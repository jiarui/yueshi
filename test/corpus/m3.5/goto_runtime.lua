-- Stripped version of lua-5.4.8-tests/goto.lua for M3.5 corpus testing.
-- Contains ONLY the portions runnable before M4 (debug lib + <close>).
-- Removed sections:
--   - Lines 155-218: debug.upvalueid assertions (M4 debug lib)
--   - Lines 253-267: <close> attribute interaction (M4 finalizers)
-- collectgarbage() at the top is a no-op stub in M3.5.

collectgarbage()

local function errmsg (code, m)
  local st, msg = load(code)
  assert(not st and string.find(msg, m))
end

-- cannot see label inside block
errmsg([[ goto l1; do ::l1:: end ]], "label 'l1'")
errmsg([[ do ::l1:: end goto l1; ]], "label 'l1'")

-- repeated label
errmsg([[ ::l1:: ::l1:: ]], "label 'l1'")
errmsg([[ ::l1:: do ::l1:: end]], "label 'l1'")


-- undefined label
errmsg([[ goto l1; local aa ::l1:: ::l2:: print(3) ]], "local 'aa'")

-- jumping over variable definition
errmsg([[
do local bb, cc; goto l1; end
local aa
::l1:: print(3)
]], "local 'aa'")

-- jumping into a block
errmsg([[ do ::l1:: end goto l1 ]], "label 'l1'")
errmsg([[ goto l1 do ::l1:: end ]], "label 'l1'")

-- cannot continue a repeat-until with variables
errmsg([[
  repeat
    if x then goto cont end
    local xuxu = 10
    ::cont::
  until xuxu < x
]], "local 'xuxu'")

-- simple gotos
local x
do
  local y = 12
  goto l1
  ::l2:: x = x + 1; goto l3
  ::l1:: x = y; goto l2
end
::l3:: ::l3_1:: assert(x == 13)


-- long labels
do
  local prog = [[
  do
    local a = 1
    goto l%sa; a = a + 1
   ::l%sa:: a = a + 10
    goto l%sb; a = a + 2
   ::l%sb:: a = a + 20
    return a
  end
  ]]
  local label = string.rep("0123456789", 40)
  prog = string.format(prog, label, label, label, label)
  assert(assert(load(prog))() == 31)
end


-- ok to jump over local dec. to end of block
do
  goto l1
  local a = 23
  x = a
  ::l1::;
end

while true do
  goto l4
  goto l1  -- ok to jump over local dec. to end of block
  goto l1  -- multiple uses of same label
  local x = 45
  ::l1:: ;;;
end
::l4:: assert(x == 13)

if print then
  goto l1   -- ok to jump over local dec. to end of block
  error("should not be here")
  goto l2   -- ok to jump over local dec. to end of block
  local x
  ::l1:: ; ::l2:: ;;
else end

-- to repeat a label in a different function is OK
local function foo ()
  local a = {}
  goto l3
  ::l1:: a[#a + 1] = 1; goto l2;
  ::l2:: a[#a + 1] = 2; goto l5;
  ::l3::
  ::l3a:: a[#a + 1] = 3; goto l1;
  ::l4:: a[#a + 1] = 4; goto l6;
  ::l5:: a[#a + 1] = 5; goto l4;
  ::l6:: assert(a[1] == 3 and a[2] == 1 and a[3] == 2 and
              a[4] == 5 and a[5] == 4)
  if not a[6] then a[6] = true; goto l3a end   -- do it twice
end

::l6:: foo()


do   -- bug in 5.2 -> 5.3.2
  local x
  ::L1::
  local y             -- cannot join this SETNIL with previous one
  assert(y == nil)
  y = true
  if x == nil then
    x = 1
    goto L1
  else
    x = x + 1
  end
  assert(x == 2 and y == true)
end

-- bug in 5.3
do
  local first = true
  local a = false
  if true then
    goto LBL
    ::loop::
    a = true
    ::LBL::
    if first then
      first = false
      goto loop
    end
  end
  assert(a)
end

do   -- compiling infinite loops
  goto escape   -- do not run the infinite loops
  ::a:: goto a
  ::b:: goto c
  ::c:: goto b
end
::escape::

-- M4 REMOVED: testing closing of upvalues (needs debug.upvalueid)
-- M4 REMOVED: if x goto optimizations testG (works but is after debug section)

--------------------------------------------------------------------------------
-- testing if x goto optimizations

local function testG (a)
  if a == 1 then
    goto l1
    error("should never be here!")
  elseif a == 2 then goto l2
  elseif a == 3 then goto l3
  elseif a == 4 then
    goto l1  -- go to inside the block
    error("should never be here!")
    ::l1:: a = a + 1   -- must go to 'if' end
  else
    goto l4
    ::l4a:: a = a * 2; goto l4b
    error("should never be here!")
    ::l4:: goto l4a
    error("should never be here!")
    ::l4b::
  end
  do return a end
  ::l2:: do return "2" end
  ::l3:: do return "3" end
  ::l1:: return "1"
end

assert(testG(1) == "1")
assert(testG(2) == "2")
assert(testG(3) == "3")
assert(testG(4) == 5)
assert(testG(5) == 10)

-- M4 REMOVED: <close> + goto back out of scope (needs <close> attribute)

print'OK'
