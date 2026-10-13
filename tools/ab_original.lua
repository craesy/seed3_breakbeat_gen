-- A/B harness: runs the ORIGINAL dnb.lua with math.random replaced by the
-- same PCG32 generator the C++ engine uses, so both make the same decisions.
-- Temporary file names keep using Lua's own generator so they don't shift
-- the sequence. Requires sox and aubioonset on PATH.
--   lua ab_original.lua <path/to/dnb.lua> <seed> [dnb.lua args...]
local dnb_path = arg[1]
local seed = math.tointeger(tonumber(arg[2]))

local MULT = 6364136223846793005
local state, inc = 0, 0
local function next32()
  local old = state
  state = old * MULT + inc
  local xs = (((old >> 18) ~ old) >> 27) & 0xffffffff
  local rot = old >> 59
  return ((xs >> rot) | (xs << ((-rot) & 31))) & 0xffffffff
end
local function pcg_seed(s)
  state = 0
  inc = (s << 1) | 1
  next32()
  state = state + (0x853c49e6748fea9b ~ s)
  next32()
end
pcg_seed(seed)

local lua_random = math.random
_G.__name_random = lua_random
math.random = function(lo, hi)
  if lo == nil then
    return (next32() >> 8) * (1.0 / 16777216.0)
  end
  if hi == nil then
    lo, hi = 1, lo
  end
  if hi <= lo then
    return lo
  end
  return lo + next32() % (hi - lo + 1)
end
math.randomseed = function() end

-- MBB_CMDLOG=<file> records every shell command dnb.lua runs.
local cmdlog = os.getenv("MBB_CMDLOG")
if cmdlog then
  local log = assert(io.open(cmdlog, "w"))
  local exec, popen = os.execute, io.popen
  local keep = os.getenv("MBB_KEEP")
  os.execute = function(c)
    log:write(c, "\n")
    log:flush()
    if keep and c:match("^rm ") then return true end
    return exec(c)
  end
  io.popen = function(c, m) log:write(c, "\n") log:flush() return popen(c, m) end
end

local f = assert(io.open(dnb_path))
local src = f:read("a")
f:close()
src = src:gsub("^#![^\n]*", "")
local patched, n = src:gsub("charset%[math%.random%(1,#charset%)%]", "charset[__name_random(1,#charset)]")
assert(n == 1, "could not patch string.random")

local args = {}
for i = 3, #arg do
  args[#args + 1] = arg[i]
end
_G.arg = args
assert(load(patched, "@dnb.lua"))()
