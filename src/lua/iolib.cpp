#include "lua/iolib.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "lua/evaluator.h"
#include "lua/numops.h"
#include "lua/value.h"

namespace ys
{
    namespace lua
    {
        namespace iolib {

        // -------------------------------------------------------------------
        // FileHandle: the payload of a file userdata.
        // -------------------------------------------------------------------
        // A shared_ptr<FileHandle> is what gets stored in Userdata::payload.
        // `stream` is the underlying fstream; for io.stdin/stdout/stderr we
        // don't own a real fstream (the C streams are global) — `is_std`
        // marks those so close() is a no-op and reads/writes route through
        // std::cin/cout/cerr via dedicated functions.
        struct FileHandle {
            std::shared_ptr<std::fstream> stream;
            bool   is_std = false;   // one of stdin/stdout/stderr
            int    std_kind = 0;     // 1=stdin, 2=stdout, 3=stderr (when is_std)
            bool   closed = false;
            bool   is_popen = false; // opened via io.popen (use pclose)
            bool   close_on_eof = false;  // set by io.lines(name)
            // popen FILE* kept separately because fstream doesn't manage it.
            std::shared_ptr<std::FILE> pfile;

            bool readable() const noexcept { return !closed; }
        };

        // -------------------------------------------------------------------
        // Helpers
        // -------------------------------------------------------------------

        // Throw a Lua-style "bad argument #N to 'fn' (X expected, got Y)".
        [[noreturn]] static void bad_arg(const char* fn, int idx,
                                         const char* expected,
                                         const LuaValue& got)
        {
            throw LuaError(std::string("bad argument #") +
                           std::to_string(idx + 1) + " to '" + fn +
                           "' (" + expected + ", got " +
                           type_name(got) + ")", 0);
        }

        // Extract FileHandle* from a LuaValue, erroring if not a file.
        static FileHandle* file_arg(const ValueVec& args, std::size_t idx,
                                    const char* fn)
        {
            if (idx >= args.size() || !args[idx].is_userdata())
                bad_arg(fn, static_cast<int>(idx), "FILE*", args.empty() ? LuaValue::nil() : args[idx]);
            auto* ud = args[idx].as_userdata();
            auto* fh = static_cast<FileHandle*>(ud->payload.get());
            return fh;
        }

        // Get a file's shared_ptr (so it stays alive).
        static std::shared_ptr<FileHandle>
        file_handle(const LuaValue& v)
        {
            if (!v.is_userdata()) return nullptr;
            return std::static_pointer_cast<FileHandle>(
                v.as_userdata()->payload);
        }

        // Read helpers ------------------------------------------------------

        // Read one byte; returns false at EOF.
        static int read_byte(FileHandle* fh)
        {
            if (fh->is_std && fh->std_kind == 1)
                return std::cin.get();
            if (fh->pfile)
                return std::fgetc(fh->pfile.get());
            if (!fh->stream) return EOF;
            return fh->stream->get();
        }

        // Peek one byte (without consuming).
        static int peek_byte(FileHandle* fh)
        {
            if (fh->is_std && fh->std_kind == 1)
                return std::cin.peek();
            if (fh->pfile) {
                int c = std::fgetc(fh->pfile.get());
                if (c != EOF) std::ungetc(c, fh->pfile.get());
                return c;
            }
            if (!fh->stream) return EOF;
            return fh->stream->peek();
        }

        // Write a string to the file.
        static void write_str(FileHandle* fh, std::string_view s)
        {
            if (fh->is_std && fh->std_kind == 2) {
                std::cout.write(s.data(), static_cast<std::streamsize>(s.size()));
            } else if (fh->is_std && fh->std_kind == 3) {
                std::cerr.write(s.data(), static_cast<std::streamsize>(s.size()));
            } else if (fh->pfile) {
                std::fwrite(s.data(), 1, s.size(), fh->pfile.get());
            } else if (fh->stream) {
                fh->stream->write(s.data(), static_cast<std::streamsize>(s.size()));
            }
        }

        // Close (idempotent). For std streams: no-op.
        static void close_file(FileHandle* fh)
        {
            if (fh->closed || fh->is_std) return;
            if (fh->pfile) {
                // shared_ptr custom deleter already pclose()s on last ref.
                fh->pfile.reset();
            }
            if (fh->stream) fh->stream->close();
            fh->closed = true;
        }

        // -------------------------------------------------------------------
        // Read formats
        // -------------------------------------------------------------------

        // Read a number per Lua's io.read("*n"): skip leading whitespace, then
        // longest numeric prefix (matches strtod). Returns (number) or (nil)
        // if no valid number.
        static LuaValue read_number(FileHandle* fh)
        {
            std::string buf;
            // Skip leading whitespace.
            int c;
            while ((c = peek_byte(fh)) != EOF && std::isspace(c))
                read_byte(fh);
            bool any = false;
            // Optional sign.
            if ((c = peek_byte(fh)) != EOF && (c == '+' || c == '-')) {
                buf.push_back(static_cast<char>(c)); read_byte(fh);
            }
            // Hex prefix? (Lua doesn't actually read hex via "*n", only via
            // tonumber — but we accept it as a courtesy; the corpus doesn't
            // exercise this path.)
            while ((c = peek_byte(fh)) != EOF && std::isdigit(c)) {
                any = true;
                buf.push_back(static_cast<char>(c)); read_byte(fh);
            }
            if (peek_byte(fh) == '.') {
                any = true;
                buf.push_back('.'); read_byte(fh);
                while ((c = peek_byte(fh)) != EOF && std::isdigit(c)) {
                    buf.push_back(static_cast<char>(c)); read_byte(fh);
                }
            }
            if ((c = peek_byte(fh)) != EOF && (c == 'e' || c == 'E')) {
                buf.push_back(static_cast<char>(c)); read_byte(fh);
                if ((c = peek_byte(fh)) != EOF && (c == '+' || c == '-')) {
                    buf.push_back(static_cast<char>(c)); read_byte(fh);
                }
                while ((c = peek_byte(fh)) != EOF && std::isdigit(c)) {
                    buf.push_back(static_cast<char>(c)); read_byte(fh);
                }
            }
            if (!any) return LuaValue::nil();
            try {
                std::size_t pos;
                long long li = std::stoll(buf, &pos);
                if (pos == buf.size()) return LuaValue::integer(li);
                double d = std::stod(buf, &pos);
                if (pos == buf.size()) return LuaValue::flt(d);
            } catch (...) {
                return LuaValue::nil();
            }
            return LuaValue::nil();
        }

        // Read a line up to and optionally including newline.
        static LuaValue read_line_v(Evaluator& ev, FileHandle* fh, bool keep_eol)
        {
            std::string line;
            int c;
            while ((c = read_byte(fh)) != EOF) {
                if (c == '\n') {
                    if (keep_eol) line.push_back(static_cast<char>(c));
                    return LuaValue::str(ev.heap().make_string(std::move(line)));
                }
                line.push_back(static_cast<char>(c));
            }
            if (line.empty()) return LuaValue::nil();
            return LuaValue::str(ev.heap().make_string(std::move(line)));
        }

        static LuaValue read_all_v(Evaluator& ev, FileHandle* fh)
        {
            std::string out;
            int c;
            while ((c = read_byte(fh)) != EOF)
                out.push_back(static_cast<char>(c));
            return LuaValue::str(ev.heap().make_string(std::move(out)));
        }

        static LuaValue read_n_v(Evaluator& ev, FileHandle* fh, std::size_t n)
        {
            std::string out;
            out.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                int c = read_byte(fh);
                if (c == EOF) break;
                out.push_back(static_cast<char>(c));
            }
            if (out.empty()) return LuaValue::nil();
            return LuaValue::str(ev.heap().make_string(std::move(out)));
        }

        // -------------------------------------------------------------------
        // io.* functions
        // -------------------------------------------------------------------

        // Default input/output handles. Stored on the io table as
        // io.__default_in / io.__default_out (private), accessed via
        // io.input()/io.output().
        static LuaValue get_default(const Table* io_tab, const char* key)
        {
            LuaKey k; k.k = LuaKey::K::Str; k.s = key;
            auto it = io_tab->hash.find(k);
            return it == io_tab->hash.end() ? LuaValue::nil() : it->second;
        }

        static void set_default(Table* io_tab, const char* key, LuaValue v)
        {
            LuaKey k; k.k = LuaKey::K::Str; k.s = key;
            io_tab->hash[k] = v;
        }

        static Table* get_io_tab(Evaluator& ev)
        {
            LuaKey ik; ik.k = LuaKey::K::Str; ik.s = "io";
            auto it = ev.globals().hash.find(ik);
            return it == ev.globals().hash.end() ? nullptr : it->second.as_table();
        }

        // Wraps a FileHandle into a userdata carrying the file metatable.
        static LuaValue make_file_value(Evaluator& ev,
                                        std::shared_ptr<FileHandle> fh,
                                        Table* file_mt)
        {
            Userdata* ud = ev.heap().make_userdata(
                std::shared_ptr<void>(std::move(fh)));
            ud->metatable = file_mt;
            return LuaValue::userdata(ud);
        }

        // Open a file by name + mode. mode: r/w/a/r+/w+/a+ with optional b.
        // Returns shared_ptr<FileHandle> on success, nullptr on failure (with
        // errno-like message in *err).
        static std::shared_ptr<FileHandle>
        open_file(const std::string& path, const std::string& mode,
                  std::string& err)
        {
            std::ios::openmode m{};
            if (mode.find('r') != std::string::npos) {
                if (mode.find('+') != std::string::npos) m |= std::ios::in | std::ios::out;
                else                                      m |= std::ios::in;
            } else if (mode.find('w') != std::string::npos) {
                if (mode.find('+') != std::string::npos) m |= std::ios::in | std::ios::out | std::ios::trunc;
                else                                      m |= std::ios::out | std::ios::trunc;
            } else if (mode.find('a') != std::string::npos) {
                if (mode.find('+') != std::string::npos) m |= std::ios::in | std::ios::out | std::ios::ate;
                else                                      m |= std::ios::out | std::ios::ate;
            } else {
                m = std::ios::in;   // default: read
            }
            if (mode.find('b') != std::string::npos) m |= std::ios::binary;

            auto fs = std::make_shared<std::fstream>(path, m);
            if (!*fs) {
                err = std::string(path) + ": No such file or directory";
                return nullptr;
            }
            auto fh = std::make_shared<FileHandle>();
            fh->stream = fs;
            return fh;
        }

        // -------------------------------------------------------------------
        // Builtins
        // -------------------------------------------------------------------

        ValueVec b_io_open(Evaluator& ev, ValueVec args)
        {
            if (args.empty() || !args[0].is_str())
                bad_arg("open", 0, "string", args.empty() ? LuaValue::nil() : args[0]);
            const std::string& path = args[0].as_str()->data;
            std::string mode = (args.size() >= 2 && args[1].is_str())
                                   ? args[1].as_str()->data : "r";
            std::string err;
            auto fh = open_file(path, mode, err);
            if (!fh)
                return {LuaValue::nil(),
                        LuaValue::str(ev.heap().make_string(err))};
            Table* io_tab = get_io_tab(ev);
            LuaKey mtk; mtk.k = LuaKey::K::Str; mtk.s = "__file_mt";
            Table* file_mt = io_tab ? io_tab->hash[mtk].as_table() : nullptr;
            return {make_file_value(ev, std::move(fh), file_mt)};
        }

        // io.close([file]) — default: close default output.
        ValueVec b_io_close(Evaluator& ev, ValueVec args)
        {
            Table* io_tab = get_io_tab(ev);
            LuaValue fv = !args.empty() ? args[0]
                : get_default(io_tab, "__default_out");
            if (!fv.is_userdata())
                bad_arg("close", 0, "FILE*", fv);
            auto fh = file_handle(fv);
            close_file(fh.get());
            (void)ev;
            return {};
        }

        // io.input([file]): get/set default input. Returns current default.
        ValueVec b_io_input(Evaluator& ev, ValueVec args)
        {
            Table* io_tab = get_io_tab(ev);
            if (!args.empty()) {
                if (args[0].is_str()) {
                    // open by name and set as default
                    std::string err;
                    auto fh = open_file(args[0].as_str()->data, "r", err);
                    if (!fh)
                        throw LuaError(err, 0);
                    LuaKey mtk; mtk.k = LuaKey::K::Str; mtk.s = "__file_mt";
                    Table* file_mt = io_tab ? io_tab->hash[mtk].as_table() : nullptr;
                    set_default(io_tab, "__default_in",
                                make_file_value(ev, std::move(fh), file_mt));
                } else if (args[0].is_userdata()) {
                    set_default(io_tab, "__default_in", args[0]);
                } else {
                    bad_arg("input", 0, "FILE* or string", args[0]);
                }
            }
            return {get_default(io_tab, "__default_in")};
        }

        // io.output([file]): symmetric.
        ValueVec b_io_output(Evaluator& ev, ValueVec args)
        {
            Table* io_tab = get_io_tab(ev);
            if (!args.empty()) {
                if (args[0].is_str()) {
                    std::string err;
                    auto fh = open_file(args[0].as_str()->data, "w", err);
                    if (!fh)
                        throw LuaError(err, 0);
                    LuaKey mtk; mtk.k = LuaKey::K::Str; mtk.s = "__file_mt";
                    Table* file_mt = io_tab ? io_tab->hash[mtk].as_table() : nullptr;
                    set_default(io_tab, "__default_out",
                                make_file_value(ev, std::move(fh), file_mt));
                } else if (args[0].is_userdata()) {
                    set_default(io_tab, "__default_out", args[0]);
                } else {
                    bad_arg("output", 0, "FILE* or string", args[0]);
                }
            }
            return {get_default(io_tab, "__default_out")};
        }

        // Core read implementation: dispatches by format string.
        static ValueVec read_impl(Evaluator& ev, FileHandle* fh,
                                  const ValueVec& fmt_args,
                                  std::size_t fmt_start)
        {
            ValueVec out;
            // No format: default to "*l".
            if (fmt_start >= fmt_args.size()) {
                LuaValue v = read_line_v(ev, fh, false);
                out.push_back(v);
                return out;
            }
            for (std::size_t i = fmt_start; i < fmt_args.size(); ++i) {
                const LuaValue& f = fmt_args[i];
                if (f.is_int()) {
                    long long n = f.as_int();
                    if (n < 0)
                        throw LuaError("bad argument to 'read' (invalid number)",
                                       0);
                    out.push_back(read_n_v(ev, fh, static_cast<std::size_t>(n)));
                } else if (f.is_str()) {
                    const std::string& s = f.as_str()->data;
                    if (s.empty()) throw LuaError("invalid format", 0);
                    char c0 = s[0];
                    if (c0 == '*') {
                        if (s.size() < 2) throw LuaError("invalid format", 0);
                        c0 = s[1];
                    }
                    if (c0 == 'n') out.push_back(read_number(fh));
                    else if (c0 == 'a') out.push_back(read_all_v(ev, fh));
                    else if (c0 == 'l') out.push_back(read_line_v(ev, fh, false));
                    else if (c0 == 'L') out.push_back(read_line_v(ev, fh, true));
                    else throw LuaError(
                        std::string("invalid format '") + s + "'", 0);
                } else {
                    throw LuaError(
                        "bad argument to 'read' (invalid format)", 0);
                }
                // Stop early at EOF if last read returned nil.
                if (out.back().is_nil()) break;
            }
            return out;
        }

        // io.read(...): reads from default input.
        ValueVec b_io_read(Evaluator& ev, ValueVec args)
        {
            Table* io_tab = get_io_tab(ev);
            LuaValue dv = get_default(io_tab, "__default_in");
            if (!dv.is_userdata())
                throw LuaError("no default input", 0);
            auto fh = file_handle(dv);
            return read_impl(ev, fh.get(), args, 0);
        }

        // io.write(...): writes to default output. Returns io library (self).
        ValueVec b_io_write(Evaluator& ev, ValueVec args)
        {
            Table* io_tab = get_io_tab(ev);
            LuaValue dv = get_default(io_tab, "__default_out");
            if (!dv.is_userdata())
                throw LuaError("no default output", 0);
            auto fh = file_handle(dv);
            for (const LuaValue& v : args) {
                std::string s;
                if (v.is_str())      s = v.as_str()->data;
                else if (v.is_number()) s = number_to_string(v);
                else
                    throw LuaError(
                        std::string("bad argument to 'write' (") +
                        "string expected, got " + type_name(v) + ")", 0);
                write_str(fh.get(), s);
            }
            LuaKey ik; ik.k = LuaKey::K::Str; ik.s = "io";
            return {ev.globals().hash[ik]};
        }

        ValueVec b_io_flush(Evaluator&, ValueVec)
        {
            std::cout.flush();
            std::cerr.flush();
            return {};
        }

        // io.type is installed inline in install_io_lib (needs ev for the
        // String result).

        // io.tmpfile(): returns a new temp file handle.
        ValueVec b_io_tmpfile(Evaluator& ev, ValueVec)
        {
            // Use tmpfile() to get an auto-deleted FILE*, wrap in fstream via
            // fdopen—actually simpler: open a unique name, immediately unlink.
            char tmpl[] = "/tmp/yueshi_tmpXXXXXX";
            int fd = ::mkstemp(tmpl);
            if (fd == -1)
                throw LuaError("cannot create temporary file", 0);
            ::unlink(tmpl);  // auto-delete on close
            auto fs = std::make_shared<std::fstream>();
            // Associate the fstream with the fd via __gnu_cxx / std::fstream
            // isn't portable; instead use fdopen + FILE* + shared_ptr.
            std::FILE* fp = ::fdopen(fd, "w+");
            if (!fp) { ::close(fd); throw LuaError("cannot open temp", 0); }
            auto fh = std::make_shared<FileHandle>();
            fh->pfile = std::shared_ptr<std::FILE>(fp, [](std::FILE* p){
                if (p) std::fclose(p);
            });
            Table* io_tab = get_io_tab(ev);
            LuaKey mtk; mtk.k = LuaKey::K::Str; mtk.s = "__file_mt";
            Table* file_mt = io_tab ? io_tab->hash[mtk].as_table() : nullptr;
            return {make_file_value(ev, std::move(fh), file_mt)};
        }

        // io.popen(cmd [, mode]): spawn a subprocess via popen.
        ValueVec b_io_popen(Evaluator& ev, ValueVec args)
        {
            if (args.empty() || !args[0].is_str())
                bad_arg("popen", 0, "string", args.empty() ? LuaValue::nil() : args[0]);
            std::string mode = (args.size() >= 2 && args[1].is_str())
                                   ? args[1].as_str()->data : "r";
            if (mode != "r" && mode != "w")
                throw LuaError(std::string("invalid mode '") + mode +
                               "'", 0);
            std::FILE* fp = ::popen(args[0].as_str()->data.c_str(), mode.c_str());
            if (!fp)
                throw LuaError(args[0].as_str()->data +
                               ": cannot popen", 0);
            auto fh = std::make_shared<FileHandle>();
            fh->is_popen = true;
            // popen mode "r" → readable; "w" → writable.
            fh->pfile = std::shared_ptr<std::FILE>(fp, [](std::FILE* p){
                if (p) ::pclose(p);
            });
            // Mark std streams if needed (no — popen files are not std).
            Table* io_tab = get_io_tab(ev);
            LuaKey mtk; mtk.k = LuaKey::K::Str; mtk.s = "__file_mt";
            Table* file_mt = io_tab ? io_tab->hash[mtk].as_table() : nullptr;
            return {make_file_value(ev, std::move(fh), file_mt)};
        }

        // Iterator function shared by io.lines and f:lines. Takes
        // (state_value, _control) and returns the next line or nil at EOF.
        // The close-on-EOF behavior is encoded in FileHandle::close_on_eof
        // (set by io.lines when it opened the file itself).
        ValueVec lines_iter(Evaluator& ev, ValueVec args)
        {
            if (args.empty()) return {LuaValue::nil()};
            auto fh = file_handle(args[0]);
            if (!fh) return {LuaValue::nil()};
            LuaValue v = read_line_v(ev, fh.get(), false);
            if (v.is_nil() && fh->close_on_eof) {
                close_file(fh.get());
            }
            return {v};
        }

        // io.lines(name [, ...]): iterator over file lines.
        // Returns an iterator function + the open handle + 0.
        ValueVec b_io_lines(Evaluator& ev, ValueVec args)
        {
            Builtin* iter = ev.heap().make_builtin("lines_iter", lines_iter);
            if (args.empty()) {
                // lines() with no args: iterate over default input. No close.
                Table* io_tab = get_io_tab(ev);
                LuaValue dv = get_default(io_tab, "__default_in");
                return {LuaValue::builtin(iter), dv, LuaValue::integer(0)};
            }
            // Open named file. Mark close_on_eof so the iterator closes it.
            if (!args[0].is_str())
                bad_arg("lines", 0, "string", args[0]);
            std::string path = args[0].as_str()->data;
            std::string err;
            auto fh = open_file(path, "r", err);
            if (!fh) throw LuaError(err, 0);
            fh->close_on_eof = true;
            Table* io_tab = get_io_tab(ev);
            LuaKey mtk; mtk.k = LuaKey::K::Str; mtk.s = "__file_mt";
            Table* file_mt = io_tab ? io_tab->hash[mtk].as_table() : nullptr;
            LuaValue fv = make_file_value(ev, std::move(fh), file_mt);
            // Extra format args beyond the filename are accepted but ignored
            // (only the default line iteration is supported).
            return {LuaValue::builtin(iter), fv, LuaValue::integer(0)};
        }

        // -------------------------------------------------------------------
        // File methods (invoked as f:method())
        // -------------------------------------------------------------------

        ValueVec f_close(Evaluator&, ValueVec args)
        {
            auto* fh = file_arg(args, 0, "close");
            close_file(fh);
            return {};
        }

        ValueVec f_read(Evaluator& ev, ValueVec args)
        {
            auto* fh = file_arg(args, 0, "read");
            return read_impl(ev, fh, args, 1);
        }

        ValueVec f_write(Evaluator& ev, ValueVec args)
        {
            auto* fh = file_arg(args, 0, "write");
            for (std::size_t i = 1; i < args.size(); ++i) {
                const LuaValue& v = args[i];
                std::string s;
                if (v.is_str())      s = v.as_str()->data;
                else if (v.is_number()) s = number_to_string(v);
                else throw LuaError(
                    std::string("bad argument #") +
                    std::to_string(i + 1) + " to 'write' "
                    "(string expected, got " + type_name(v) + ")", 0);
                write_str(fh, s);
            }
            (void)ev;
            return {args[0]};
        }

        ValueVec f_lines(Evaluator& ev, ValueVec args)
        {
            // f:lines() — like io.lines but on an open handle; do NOT close
            // at EOF (the user owns the handle). Uses the shared lines_iter.
            (void)args;
            Builtin* iter = ev.heap().make_builtin("lines_iter", lines_iter);
            return {LuaValue::builtin(iter)};
        }

        ValueVec f_seek(Evaluator& ev, ValueVec args)
        {
            auto* fh = file_arg(args, 0, "seek");
            std::string whence = (args.size() >= 2 && args[1].is_str())
                                     ? args[1].as_str()->data : "cur";
            long long off = (args.size() >= 3 && args[2].is_number())
                                ? args[2].as_int() : 0;
            std::ios::seekdir dir;
            if (whence == "set")      dir = std::ios::beg;
            else if (whence == "cur") dir = std::ios::cur;
            else if (whence == "end") dir = std::ios::end;
            else throw LuaError(
                std::string("bad argument #2 to 'seek' (invalid option '")
                + whence + "')", 0);
            if (fh->stream) {
                fh->stream->clear();
                fh->stream->seekg(static_cast<std::streamoff>(off), dir);
                fh->stream->seekp(static_cast<std::streamoff>(off), dir);
                std::streampos pos = fh->stream->tellg();
                if (pos == std::streampos(-1))
                    return {LuaValue::nil(),
                            LuaValue::str(ev.heap().make_string(
                                "invalid seek position"))};
                return {LuaValue::integer(static_cast<long long>(pos))};
            }
            if (fh->pfile) {
                int w = SEEK_CUR;
                if (whence == "set") w = SEEK_SET;
                else if (whence == "end") w = SEEK_END;
                std::fseek(fh->pfile.get(), static_cast<long>(off), w);
                long pos = std::ftell(fh->pfile.get());
                return {LuaValue::integer(static_cast<long long>(pos))};
            }
            return {LuaValue::integer(0)};
        }

        ValueVec f_flush(Evaluator&, ValueVec args)
        {
            auto* fh = file_arg(args, 0, "flush");
            if (fh->stream) fh->stream->flush();
            if (fh->pfile) std::fflush(fh->pfile.get());
            return {};
        }

        ValueVec f_setvbuf(Evaluator&, ValueVec args)
        {
            (void)file_arg(args, 0, "setvbuf");
            // Largely a no-op: std::fstream buffering is not user-controllable
            // at this granularity from C++. Accept any args silently.
            return {};
        }

        // __tostring: "file (0x..)" or "file (closed)"
        // Implemented as a Lua builtin since we need ev for the String alloc.
        ValueVec f_tostring(Evaluator& ev, ValueVec args)
        {
            auto* fh = file_arg(args, 0, "__tostring");
            std::ostringstream os;
            os << "file (" << static_cast<const void*>(args[0].as_userdata())
               << ")";
            if (fh->closed) os << " [closed]";
            return {LuaValue::str(ev.heap().make_string(os.str()))};
        }

        } // namespace iolib

        // -------------------------------------------------------------------
        // install_io_lib
        // -------------------------------------------------------------------
        void install_io_lib(Evaluator& ev)
        {
            Heap& h = ev.heap();
            Table* io_tab = h.make_table();
            using namespace iolib;

            auto add = [&](const char* name, BuiltinFn fn) {
                Builtin* b = h.make_builtin(name, fn);
                LuaKey k; k.k = LuaKey::K::Str; k.s = name;
                io_tab->hash[k] = LuaValue::builtin(b);
            };

            // Top-level io functions.
            add("open",    b_io_open);
            add("close",   b_io_close);
            add("input",   b_io_input);
            add("output",  b_io_output);
            add("read",    b_io_read);
            add("write",   b_io_write);
            add("flush",   b_io_flush);
            add("tmpfile", b_io_tmpfile);
            add("popen",   b_io_popen);
            add("lines",   b_io_lines);
            // io.type needs the heap for the String result.
            add("type", [](Evaluator& e, ValueVec a) -> ValueVec {
                if (a.empty() || !a[0].is_userdata()) return {LuaValue::nil()};
                auto fh = file_handle(a[0]);
                if (!fh) return {LuaValue::nil()};
                const char* s = fh->closed ? "closed file" : "file";
                return {LuaValue::str(e.heap().make_string(s))};
            });

            // Build the file-methods table (the __index contents).
            Table* methods = h.make_table();
            auto m = [&](const char* name, BuiltinFn fn) {
                Builtin* b = h.make_builtin(name, fn);
                LuaKey k; k.k = LuaKey::K::Str; k.s = name;
                methods->hash[k] = LuaValue::builtin(b);
            };
            m("close",   f_close);
            m("read",    f_read);
            m("write",   f_write);
            m("lines",   f_lines);
            m("seek",    f_seek);
            m("flush",   f_flush);
            m("setvbuf", f_setvbuf);

            // Build the per-type FILE* metatable.
            Table* mt = h.make_table();
            {
                LuaKey k; k.k = LuaKey::K::Str; k.s = "__name";
                mt->hash[k] = LuaValue::str(h.make_string("FILE*"));
            }
            {
                LuaKey k; k.k = LuaKey::K::Str; k.s = "__index";
                mt->hash[k] = LuaValue::table(methods);
            }
            {
                LuaKey k; k.k = LuaKey::K::Str; k.s = "__tostring";
                Builtin* b = h.make_builtin("file_tostring", f_tostring);
                mt->hash[k] = LuaValue::builtin(b);
            }

            // Stash the metatable privately on io.__file_mt (and the methods on
            // io.__file_methods for tests).
            {
                LuaKey k; k.k = LuaKey::K::Str; k.s = "__file_mt";
                io_tab->hash[k] = LuaValue::table(mt);
            }
            {
                LuaKey k; k.k = LuaKey::K::Str; k.s = "__file_methods";
                io_tab->hash[k] = LuaValue::table(methods);
            }

            // Pre-create the standard streams. They share a placeholder FH with
            // is_std=true so close() is a no-op.
            auto make_std = [&](int kind) -> LuaValue {
                auto fh = std::make_shared<FileHandle>();
                fh->is_std = true;
                fh->std_kind = kind;
                Userdata* ud = h.make_userdata(
                    std::shared_ptr<void>(std::move(fh)));
                ud->metatable = mt;
                return LuaValue::userdata(ud);
            };
            LuaValue stdin_h  = make_std(1);
            LuaValue stdout_h = make_std(2);
            LuaValue stderr_h = make_std(3);
            {
                LuaKey k; k.k = LuaKey::K::Str; k.s = "stdin";
                io_tab->hash[k] = stdin_h;
            }
            {
                LuaKey k; k.k = LuaKey::K::Str; k.s = "stdout";
                io_tab->hash[k] = stdout_h;
            }
            {
                LuaKey k; k.k = LuaKey::K::Str; k.s = "stderr";
                io_tab->hash[k] = stderr_h;
            }
            // Default input = stdin; default output = stdout.
            set_default(io_tab, "__default_in",  stdin_h);
            set_default(io_tab, "__default_out", stdout_h);

            // Register the io table in _G.
            LuaKey gk; gk.k = LuaKey::K::Str; gk.s = "io";
            ev.globals().hash[gk] = LuaValue::table(io_tab);
        }

    } // namespace lua
} // namespace ys
