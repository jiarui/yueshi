#pragma once

// yueshi io library (M3.4-B): file handles + io.* + file methods.
//
// File handles are full userdata (Userdata GCObject) whose payload is a
// std::shared_ptr<FileHandle>. The shared_ptr lets io.input()/io.output()
// alias the default handle without copying the underlying stream, and gives
// the GC a deterministic cleanup path (reset on sweep) ahead of M4's __gc
// finalizer integration.
//
// The per-type FILE* metatable is built once at install time and shared by
// every file handle. It carries __name = "FILE*" (for error messages) and
// __index = the file-methods table (read/write/close/lines/seek/flush/setvbuf).

namespace ys
{
    namespace lua
    {
        class Evaluator;

        // Create and install the io library. Called once from
        // Evaluator::install_builtins().
        void install_io_lib(Evaluator& ev);
    }
}
