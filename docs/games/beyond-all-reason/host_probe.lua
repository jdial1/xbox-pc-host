-- Xbox PC Host test probe: logs frame rate and sim speed every period and takes screenshots, so a run
-- with no desktop can be judged from the log. With HostProbeBisect=1 in springsettings it also bisects
-- the widgets: each period it disables a set, and keeps narrowing the set that restores the frame rate.
function widget:GetInfo()
	return { name = "Host Probe", desc = "FPS log, widget bisection, timed screenshots", layer = -10000, enabled = true, handler = true }
end

local PERIOD = 6
local shots = { [30 * 20] = true }
local frames, last = 0, nil
local bisect = Spring.GetConfigInt("HostProbeBisect", 0) == 1
local nowidgets = Spring.GetConfigInt("HostProbeNoWidgets", 0) == 1 -- screenshot with every other widget off
local luauiOff = Spring.GetConfigInt("HostProbeLuaUIOff", 0) == 1 -- turn LuaUI off mid-game (this widget goes too)
local allNames = nil
local baseline, suspects, testing, phase = nil, nil, nil, 0

local function Names()
	local list = {}
	for n, k in pairs(widgetHandler.knownWidgets or {}) do
		if k.active and n ~= "Host Probe" then list[#list + 1] = n end
	end
	table.sort(list)
	Spring.Echo("[host-probe] bisect: widgets list has " .. #(widgetHandler.widgets or {}) .. " entries")
	return list
end

local function SetEnabled(set, on)
	for _, n in ipairs(set) do
		if on then widgetHandler:EnableWidget(n) else widgetHandler:DisableWidget(n) end
	end
end

local function Half(list)
	local a = {}
	for i = 1, math.ceil(#list / 2) do a[#a + 1] = list[i] end
	return a
end

local function Without(list, remove)
	local skip, out = {}, {}
	for _, n in ipairs(remove) do skip[n] = true end
	for _, n in ipairs(list) do if not skip[n] then out[#out + 1] = n end end
	return out
end

local function Step(fps)
	phase = phase + 1
	if phase < 3 then return end -- let the game settle
	if not baseline then
		baseline = fps
		suspects = Names()
		testing = suspects
		Spring.Echo(string.format("[host-probe] bisect: baseline %.1f fps, %d widgets; disabling all", fps, #suspects))
		SetEnabled(testing, false)
		return
	end
	SetEnabled(testing, true)
	local fixed = fps > baseline * 3
	Spring.Echo(string.format("[host-probe] bisect: %d disabled -> %.1f fps (%s)", #testing, fps, fixed and "faster" or "same"))
	if testing == suspects and not fixed then
		Spring.Echo("[host-probe] bisect: disabling every widget does not help; the cost is outside LuaUI")
		bisect = false
		return
	end
	if fixed then suspects = testing else suspects = Without(suspects, testing) end
	if #suspects == 1 then
		Spring.Echo("[host-probe] bisect: culprit " .. suspects[1])
		bisect = false
		return
	end
	testing = Half(suspects)
	if #testing <= 4 then Spring.Echo("[host-probe] bisect: testing " .. table.concat(testing, ", ")) end
	SetEnabled(testing, false)
end

function widget:Initialize()
	last = Spring.GetTimer()
end

function widget:DrawScreen()
	frames = frames + 1
	local now = Spring.GetTimer()
	local dt = Spring.DiffTimers(now, last)
	if dt >= PERIOD then
		local fps = frames / dt
		local _, speed = Spring.GetGameSpeed()
		Spring.Echo(string.format("[host-probe] fps=%.1f gameframe=%d units=%d speedfactor=%.2f",
			fps, Spring.GetGameFrame(), #Spring.GetAllUnits(), speed or 0))
		frames, last = 0, now
		if bisect then Step(fps) end
		phase = bisect and phase or phase + 1
		if not bisect and luauiOff and phase == 3 then
			Spring.Echo("[host-probe] disabling LuaUI")
			Spring.SendCommands("luaui disable")
		end
		if not bisect and nowidgets then
			if phase == 3 then allNames = Names(); SetEnabled(allNames, false); Spring.Echo("[host-probe] all " .. #allNames .. " widgets disabled") end
			if phase == 4 then Spring.SendCommands("screenshot png"); Spring.Echo("[host-probe] screenshot with widgets disabled") end
			if phase == 5 then SetEnabled(allNames, true); Spring.Echo("[host-probe] widgets re-enabled") end
		end
		if not bisect and Spring.GetConfigInt("HostProbeDebug", 0) == 1 then
			if phase == 3 then Spring.SendCommands("debug") end
			if phase == 6 then Spring.SendCommands("screenshot png"); Spring.Echo("[host-probe] debug screenshot") end
		end
	end
end

function widget:GameFrame(f)
	if shots[f] then
		Spring.SendCommands("screenshot png")
		Spring.Echo("[host-probe] screenshot at frame " .. f)
	end
end
