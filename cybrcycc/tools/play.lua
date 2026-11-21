-- the same test rider as cc_game.c autoplay (CC_AUTO=0)
local s = manager.machine.screens:at(1)
local io = manager.machine.ioport
local inp = io.ports[":INPUTS"]
local coin, start
for name, f in pairs(inp.fields) do
  if f.type == io:token_to_input_type("COIN1") then coin = f end
  if f.type == io:token_to_input_type("START1") then start = f end
end
local bank = io.ports[":ADC.0"].fields["Bike Bank"]
local thr = io.ports[":ADC.1"].fields["Throttle Grip"]
local n = 0
emu.register_frame_done(function()
  n = n + 1
  coin:set_value((n % 600 >= 300 and n % 600 < 306) and 1 or 0)
  start:set_value((n % 300 >= 150 and n % 300 < 156 and n > 400) and 1 or 0)
  thr:set_value(n > 700 and 0x300 or 0)
  bank:set_value((n // 90) % 2 == 0 and 0x180 or 0x280)
  if n % 60 == 0 and n >= 1200 then s:snapshot(string.format("f%05d.png", n)) end
  if n >= 5400 then manager.machine:exit() end
end)
