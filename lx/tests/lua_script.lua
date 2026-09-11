-- lx/tests/lua_script.lua — the LX_COMPAT L5 gate program.
--
-- The flagship rung of the ladder: an UNMODIFIED interpreter (stock
-- `make linux` lua 5.4, a PIE linked against glibc + libm) runs a real
-- script.  Every section exercises a different part of the runtime so a
-- failure lands on a named assert, and the final receipt is greppable
-- from the serial log by test_lx_lua.sh:
--
--   1. arithmetic  — the math operators + the `math` library;
--   2. strings     — the string library (upper/sub/format/length);
--   3. table       — a 10 000-entry table built, sorted and reduced
--                    (allocation + table.sort + ipairs);
--   4. os.date     — the C library's time/date path (gettimeofday /
--                    clock_gettime under the hood);
--   5. io.lines    — reads a REAL file under /linux (the staged motd),
--                    proving file I/O + the /linux prefix resolve.

-- 1) arithmetic
assert(2 + 3 * 4 == 14, "arithmetic")
assert(2 ^ 10 == 1024, "exponent")
assert(17 % 5 == 2 and -17 % 5 == 3, "modulo sign")
assert(math.floor(7 / 2) == 3 and math.sqrt(144) == 12, "math lib")

-- 2) strings
assert(("lua"):upper() == "LUA", "string.upper")
assert(("AuraLite"):sub(5) == "Lite", "string.sub")
assert(string.format("%s-%d", "lua", 54) == "lua-54", "string.format")
assert(#("hello") == 5, "length operator")

-- 3) table stress: 10 000 entries, sorted, reduced
local t = {}
for i = 1, 10000 do t[i] = (i * 7919) % 100000 end
table.sort(t)
local sum = 0
for i = 1, #t do sum = sum + t[i] end
assert(#t == 10000 and t[1] <= t[2] and t[#t] >= t[1], "table.sort")
assert(sum == 499895000, "table reduce")   -- host-computed, exact

-- 4) os.time + os.date (the C library's time/date path).  The guest
--    RTC is deliberately read as epoch 0 (time_init_cmos), so the year
--    is "1970" — what is asserted is that the path WORKS: time() returns
--    a number and date() formats it, not any particular wall-clock year.
local now = os.time()
assert(type(now) == "number" and now >= 0, "os.time")
local year = os.date("%Y", now)
assert(type(year) == "string" and #year == 4, "os.date %Y")
assert(tonumber(year) ~= nil, "os.date year parses")

-- 5) io.lines over a real /linux file (the staged motd)
local lines = 0
for _ in io.lines("/linux/etc/motd") do lines = lines + 1 end
assert(lines >= 1, "io.lines read /linux/etc/motd")

print("LX5-LUA-OK")
