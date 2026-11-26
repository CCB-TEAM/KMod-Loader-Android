-- card_art: pick one of several art variants for a card on the card detail page.
-- Variants come from "art packs": kmod/art/<pack>/index.json + PNG files (made by tools/kmod-art from patch paks).
-- The game paints a 2D card with a material (parameter "CardImage"); we swap that texture and keep the choice per card art.

local ART = kmod.paths.root .. "/art"
local DATA = kmod.paths.root .. "/data/" .. Mod.id
kmod.mkdirs(DATA)
local CHOICES = DATA .. "/choices.json"
local META = DATA .. "/packs.json"
local NAMES = DATA .. "/names.json"
-- decoded GIF frames go to the app's private folder, which no gallery app can see; older versions kept them in shared storage
local CACHE = (kmod.paths.private and (kmod.paths.private .. "/cache/card_art")) or (DATA .. "/cache")
if CACHE ~= DATA .. "/cache" and kmod.exists(DATA .. "/cache") then pcall(kmod.rmtree, DATA .. "/cache") end
-- the player's own pictures stay in shared storage (to be shared and backed up), hidden from the gallery
kmod.mkdirs(ART)
for _, d in ipairs({ ART, DATA }) do if not kmod.exists(d .. "/.nomedia") then kmod.writeFile(d .. "/.nomedia", "") end end
local PARAM = "CardImage"
local CLICK = "WBP_NUI_MasterTextButton_C:BndEvt__Button_0_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature"

local packs, byAsset, choices = {}, {}, {}
local names = {}      -- names[asset][language] = the card's name, learned when its detail page is shown
local texCache, state, buttons, importButtons, manageButtons = {}, {}, {}, {}, {}
local packInfo, disabled = {}, {}      -- packInfo[pack] = { entries = index.json list, images, cards }; disabled[pack] = true: the pack is switched off
local pending = {}      -- freshly created card objects waiting for their material (see the loops at the end)
local tracked = {}      -- addr -> the cards that have variants: re-checked every 50 ms (see the loops at the end)

-- ---------------------------------------------------------------- data
local function scan()
  packs, byAsset, packInfo = {}, {}, {}
  for _, e in ipairs(kmod.listdir(ART)) do
    if e.dir then
      local dir = ART .. "/" .. e.name
      local txt = kmod.readFile(dir .. "/index.json")
      local idx = txt and kmod.json.decode(txt)
      if type(idx) == "table" then
        packs[#packs + 1] = e.name
        local seen, cards = {}, 0
        for _, it in ipairs(idx) do
          if not tostring(it.File):lower():match("%.mp4$") then                       -- videos are no longer supported
            local key = tostring(it.Asset):lower()
            if not seen[key] then cards = cards + 1 end
            seen[key] = (seen[key] or 0) + 1
            local n = seen[key]
            if not disabled[e.name] then
              byAsset[key] = byAsset[key] or {}
              -- a variant is named after its pack; the 2nd, 3rd... image of the same card in one pack get "#2", "#3"
              table.insert(byAsset[key], { pack = e.name, n = n, key = n == 1 and e.name or (e.name .. "#" .. n), file = dir .. "/" .. it.File, crop = it.Crop })
            end
          end
        end
        packInfo[e.name] = { entries = idx, images = #idx, cards = cards }
      end
    end
  end
  table.sort(packs)
  for _, list in pairs(byAsset) do
    table.sort(list, function(a, b) if a.pack ~= b.pack then return a.pack < b.pack end return a.n < b.n end)
  end
  local n = 0
  for _ in pairs(byAsset) do n = n + 1 end
  Log.info(#packs .. " art pack(s), " .. n .. " asset(s) with variants")
end

local function loadChoices()
  local txt = kmod.readFile(CHOICES)
  local t = txt and kmod.json.decode(txt)
  choices = type(t) == "table" and t or {}
end
local function saveChoices() kmod.writeFile(CHOICES, kmod.json.encode(choices)) end

-- "/Game/Assets/Textures/Images/Germany/t_pnz_35t.t_pnz_35t" -> "/game/assets/textures/images/germany/t_pnz_35t"
local function assetOf(tex)
  local p = tex:GetPathName()
  return (p:match("^(.-)%.[^./:]+$") or p):lower()
end

local lib = StaticFindObject("/Script/Engine.Default__KismetRenderingLibrary")
-- a GIF: the loader cuts it into PNG frames (cached under data/card_art/cache); a card shows the frame that matches the clock
local anims = {}
local gifSeen = {}
-- seconds on the wall (os.clock counts processor time on Android, so a GIF would run at the wrong, uneven speed)
local clock = {}
local function realSeconds()
  if not (clock.lib and clock.lib:IsValid()) then clock.lib = StaticFindObject("/Script/Engine.Default__GameplayStatics") end
  if not (clock.pc and clock.pc:IsValid()) then clock.pc = FindFirstOf("PlayerController") end
  return clock.lib:GetRealTimeSeconds(clock.pc)
end
local function gifAnim(file, crop)
  local cx, cy, cs
  if type(crop) == "table" and tonumber(crop.x) and tonumber(crop.y) and tonumber(crop.s) then cx, cy, cs = math.floor(crop.x), math.floor(crop.y), math.floor(crop.s) end
  local ck = file .. (cs and (":" .. cx .. "," .. cy .. "," .. cs) or "")
  local a = anims[ck]
  if a and a.frames[1]:IsValid() then return a end
  if a then Log.warn("gif animation lost its frames, rebuilding: " .. ck)
  elseif gifSeen[ck] then Log.warn("gif animation rebuilt: " .. ck) end
  gifSeen[ck] = true
  local key = (ck:gsub("[^%w]", "_"))
  local dir = CACHE .. "/" .. key:sub(-90) .. "_" .. #ck
  local info
  local meta = kmod.readFile(dir .. "/frames.json")
  if meta then info = kmod.json.decode(meta) end
  if not (type(info) == "table" and type(info.delays) == "table" and #info.delays > 0) then
    kmod.mkdirs(dir)
    local r, err = kmod.gif_frames(file, dir, 480, 60, cx, cy, cs)
    if not r then Log.warn("gif " .. file .. ": " .. tostring(err)); return nil end
    info = r
    kmod.writeFile(dir .. "/frames.json", kmod.json.encode(r))
  end
  local frames, total = {}, 0
  for i = 1, #info.delays do
    local t = lib:ImportFileAsTexture2D(FindFirstOf("PlayerController"), dir .. string.format("/f%03d.png", i))
    if not t:IsValid() then Log.warn("gif frame " .. i .. " could not be imported"); return nil end
    frames[i] = t
    total = total + info.delays[i]
  end
  a = { frames = frames, delays = info.delays, total = math.max(total, 1), t0 = realSeconds(), pinned = {} }
  anims[ck] = a
  Log.info("gif " .. file .. ": " .. #frames .. " frames, " .. total .. " ms")
  return a
end
local function animFrame(a)
  local t = ((realSeconds() - a.t0) * 1000) % a.total
  for i, d in ipairs(a.delays) do
    if t < d then return a.frames[i] end
    t = t - d
  end
  return a.frames[1]
end

local function texture(file)
  local t = texCache[file]
  if t and t:IsValid() then return t end
  t = lib:ImportFileAsTexture2D(FindFirstOf("PlayerController"), file)
  if t:IsValid() then texCache[file] = t; return t end
  Log.warn("could not import " .. file)
end

-- ---------------------------------------------------------------- applying a variant to a 2D card
local function variantFile(asset, key)
  for _, v in ipairs(byAsset[asset] or {}) do if v.key == key then return v.file, v.crop end end
end

-- mat: the card's material instance (MID); orig: optional, the game's own texture for this card
local function applyMat(mat, refresh, orig, param)
  param = param or PARAM
  if not (mat and mat:IsValid()) then return end
  local cur = mat:K2_GetTextureParameterValue(param)
  local addr = mat:GetAddress()
  if orig and orig:IsValid() and orig:GetPathName():find("^/Game/") then
    local a = assetOf(orig)
    if not state[addr] or state[addr].asset ~= a then state[addr] = { asset = a, orig = orig } end
  elseif cur:IsValid() and cur:GetPathName():find("^/Game/") then           -- the game just (re)set the card's own art
    state[addr] = { asset = assetOf(cur), orig = cur }
  end
  local st = state[addr]
  if not st then return end
  if byAsset[st.asset] then tracked[addr] = { mat = mat, refresh = refresh, param = param } else tracked[addr] = nil end
  local target = st.orig
  local want = choices[st.asset]
  if want then
    local file, crop = variantFile(st.asset, want)
    local t
    if file and file:lower():match("%.gif$") then
      local a = gifAnim(file, crop)
      t = a and animFrame(a)
      if a then                                       -- the card's material refers to every frame: the engine's garbage collector frees what nothing refers to
        local pm = a.pinned[addr]
        if not (pm and pm:IsValid() and pm == mat) then      -- a new material (the game replaces them): pin again at once
          a.pinned[addr] = mat
          for i, f in ipairs(a.frames) do pcall(function() mat:SetTextureParameterValue("KmodAnim" .. i, f) end) end
        end
      end
    else t = file and texture(file) end
    if t then target = t end
  end
  if target and target:IsValid() and not (cur:IsValid() and cur == target) then
    mat:SetTextureParameterValue(param, target)
    if refresh then refresh() end
  end
  return st.asset
end

-- a 2D card widget (detail page, lists): the material is painted by an Image widget, which must be told to repaint
local function apply(w)
  local mat = w.mat
  return applyMat(mat, function() pcall(function() w.Image:SetBrushFromMaterial(mat) end) end)
end

-- 3D card actors (collection grid, hand, battlefield): CardMaterial on the CardV2 mesh, drawn every frame
local CARD3D = { "BP_HandCardLook_C", "BP_BaseCard_C", "BP_HandCard_C", "BP_DraftingCard_C" }
local function apply3D(h)
  local ok, mat = pcall(function() return h.CardMaterial end)
  if not (ok and mat and mat:IsValid()) then return end
  local okr, ref = pcall(function() return h.imageObjectReference end)
  return applyMat(mat, nil, okr and ref or nil)
end
-- deck builder rows (W_CollectionCardDeckWidget_C): a small Image whose material has the parameter "mainImage"
local function applyRow(row)
  local im = row.thumbnailimage
  local ok, b = pcall(function() return im.Brush end)
  local mat = ok and b and b.ResourceObject
  if not (mat and mat:IsValid() and mat:GetClass():GetName() == "MaterialInstanceDynamic") then return end
  local okr, ref = pcall(function() return row.imageObjectReference end)
  return applyMat(mat, function() pcall(function() im:SetBrushFromMaterial(mat) end) end, okr and ref or nil, "mainImage")
end

local function applyAll3D()
  if next(byAsset) == nil then return end
  for _, row in ipairs(FindAllOf("W_CollectionCardDeckWidget_C") or {}) do
    local ok, err = pcall(applyRow, row)
    if not ok then Log.warn("applyRow: " .. tostring(err)) end
  end
  for _, cn in ipairs(CARD3D) do
    for _, h in ipairs(FindAllOf(cn) or {}) do
      local ok, err = pcall(apply3D, h)
      if not ok then Log.warn("apply3D: " .. tostring(err)) end
    end
  end
end

-- ---------------------------------------------------------------- the switch button on the detail page
local function variants(asset)
  local list = { "" }                                                -- "" = the game's own art
  for _, v in ipairs(byAsset[asset] or {}) do list[#list + 1] = v.key end
  return list
end

-- "pack" or "pack#2" -> what the button shows
local function showName(key)
  local p, n = key:match("^(.*)#(%d+)$")
  if p then return p .. " (" .. n .. ")" end
  return key
end

local function label(asset)
  local cur = choices[asset]
  return Lang.t("art") .. ": " .. ((cur and cur ~= "") and showName(cur) or Lang.t("original"))
end

-- The card detail page exists in two layouts: the mobile one (Android) and the PC one (Windows).
local DETAIL = { "W_CollectionCardDetail_Mobile_C", "W_CollectionCardDetail_PC_C" }
local DETAIL_SET = {}
for _, n in ipairs(DETAIL) do DETAIL_SET[n] = true end

local function detailOf(w)
  local ok, det = pcall(function() return w:GetOuter():GetOuter() end)
  if ok and det:IsValid() and DETAIL_SET[det:GetClass():GetName()] then return det end
end

-- the page's 2D card widget: W_Card2D on the mobile page, thisCard2D on the PC page (tried in this order)
local function cardOf(det)
  for _, name in ipairs({ "W_Card2D", "thisCard2D" }) do
    local ok, w = pcall(function() return det[name] end)
    if ok and w and w:IsValid() then return w end
  end
end

local function learnName(det, asset)
  local ok, text = pcall(function()
    local t = det.cardHeading:GetText()
    if type(t) == "string" then return t end
    local ok2, s2 = pcall(function() return t:ToString() end)
    return ok2 and s2 or tostring(t)
  end)
  if not ok then Log.warn("card name: " .. tostring(text)); return end
  text = tostring(text)
  if text == "" then Log.debug("card name: empty heading"); return end
  Log.debug("card name of " .. asset .. ": " .. text)
  local lang = Lang.current()
  local e = names[asset] or {}
  if e[lang] ~= text then e[lang] = text; names[asset] = e; kmod.writeFile(NAMES, kmod.json.encode(names)) end
end

local function refreshButton(det, asset, learn)
  if learn and asset then
    local okL, errL = pcall(learnName, det, asset)
    if not okL then Log.warn("card name: " .. tostring(errL)) end
  end
  local ib = importButtons[det:GetAddress()]
  if ib and ib:IsValid() then                                        -- "Import": any card whose art we know can get an imported variant
    pcall(function() ib:SetVisibility(asset and 0 or 1) end)
    if asset then ib:SetText(Lang.t("import")) end
  end
  local b = buttons[det:GetAddress()]
  if not (b and b:IsValid()) then return end
  local mb = manageButtons[det:GetAddress()]
  if mb and mb:IsValid() then
    pcall(function() mb:SetVisibility(asset and 0 or 1) end)
    if asset then mb:SetText(Lang.t("manage")) end
  end
  if asset and #variants(asset) > 1 then
    pcall(function() b:SetVisibility(0) end)
    b:SetText(label(asset))
  else
    pcall(function() b:SetVisibility(1) end)                         -- Collapsed: no variants for this card
  end
end

-- a new text button (the game's own style) placed where the page keeps its buttons / texts
local rows = {}      -- det addr -> the HorizontalBox of our own buttons
local function makeButton(det, what)
  local cls = FindObject("WidgetBlueprintGeneratedClass", "WBP_NUI_MasterTextButton_C")
  local wl = StaticFindObject("/Script/UMG.Default__WidgetBlueprintLibrary")
  local b = wl:Create(det, cls, FindFirstOf("PlayerController"))
  local added = false
  -- our buttons get a row of their own under the page's own button row (it already holds "Info" and "History"; five on one row are too small)
  local ok0, row = pcall(function()
    local old = rows[det:GetAddress()]
    if old and old:IsValid() then return old end
    local hbox = det.widgetSwitcherButtonHBox
    local parent = hbox:GetParent()
    local gp = StaticFindObject("/Script/Engine.Default__GameplayStatics")
    local tree = det.WidgetTree
    local vbox = gp:SpawnObject(StaticFindObject("/Script/UMG.VerticalBox"), tree)
    vbox:AddChild(hbox)                                            -- moves the page's own row into the new box
    parent:SetContent(vbox)
    local r = gp:SpawnObject(StaticFindObject("/Script/UMG.HorizontalBox"), tree)
    vbox:AddChild(r)
    rows[det:GetAddress()] = r
    Log.info("button row: added a second row")
    return r
  end)
  if ok0 and row then
    local slot
    if pcall(function() slot = row:AddChild(b) end) then
      pcall(function() slot:SetSize({ SizeRule = 1, Value = 1 }) end)
      added = true
      Log.info(what .. " button goes into its own row")
    end
  elseif not ok0 then Log.warn("no extra button row: " .. tostring(row)) end
  for _, name in ipairs(added and {} or { "widgetSwitcherButtonHBox", "infoVBox", "ArtistVBox" }) do
    local ok, box = pcall(function() return det[name] end)
    if ok and box and box:IsValid() and pcall(function() box:AddChild(b) end) then added = true; Log.info(what .. " button goes into " .. name); break end
  end
  if not added then Log.warn("no place for the " .. what .. " button on " .. det:GetName()) end
  return b
end

local function ensureButton(det)
  local addr = det:GetAddress()
  local b = buttons[addr]
  if b and b:IsValid() then return b end
  b = makeButton(det, "art")
  buttons[addr] = b
  Log.info("added the art button to " .. det:GetName())
  if Media then                                                       -- the loader can show an image chooser (gallery / file dialog)
    local ib = makeButton(det, "import")
    importButtons[addr] = ib
    pcall(function() ib:SetText(Lang.t("import")) end)
  end
  if UI then
    local mb = makeButton(det, "manage")
    manageButtons[addr] = mb
    pcall(function() mb:SetText(Lang.t("manage")) end)
  end
  return b
end

-- Hooks only see functions the ENGINE calls (events, delegates). Calls inside one Blueprint (like SetCardImage from the
-- detail page's own graph) never pass through ProcessEvent, so we hook the entry points instead:
--   * W_CollectionCardDetail_Mobile_C:Construct / UpdateCollectionCardVisuals : the page opens or shows another card
--   * W_Card2D_C:OnLoaded_*                                                    : the card's art finished loading (async)
local function onCard2D(w)
  local asset = apply(w)
  local det = detailOf(w)
  if det then ensureButton(det); refreshButton(det, asset, true) end
end
local function onDetail(det)
  ensureButton(det)
  local w = cardOf(det)
  refreshButton(det, w and apply(w) or nil, true)
end

local cardCls = FindObject("WidgetBlueprintGeneratedClass", "W_Card2D_C")
local hooked = 0
for _, f in ipairs(cardCls:GetFunctions()) do
  local fn = f:match("^W_Card2D_C%.(OnLoaded_.+)$")
  if fn then
    RegisterHook("W_Card2D_C:" .. fn, function() end, function(self) Log.debug("OnLoaded"); onCard2D(self) end)
    hooked = hooked + 1
  end
end
Log.info("hooked " .. hooked .. " OnLoaded callback(s) of W_Card2D_C")
RegisterHook("W_Card2D_C:SetCardImage", function() end, function(self) onCard2D(self) end)
for _, cn in ipairs(DETAIL) do
  for _, fn in ipairs({ "Construct", "UpdateCollectionCardVisuals" }) do
    local ok, err = pcall(RegisterHook, cn .. ":" .. fn, function() end, function(self) Log.debug(cn .. " " .. fn); onDetail(self) end)
    if not ok then Log.warn("cannot hook " .. cn .. ":" .. fn .. " (" .. tostring(err):match("^[^"..string.char(10).."]*") .. ")") end
  end
end

-- ---------------------------------------------------------------- importing images: a window of its own
local function toast(text) pcall(function() UI.toast(text, 4) end) end

local function readIndex(dir)
  local txt = kmod.readFile(dir .. "/index.json")
  local idx = txt and kmod.json.decode(txt)
  return type(idx) == "table" and idx or {}
end

-- a folder name that is safe on both platforms
local function cleanName(s)
  s = tostring(s or ""):gsub('[\\/:*?"<>|%c]', ""):gsub("^%s+", ""):gsub("%s+$", "")
  if #s > 48 then s = s:sub(1, 48) end
  return s
end

local function stemTaken(dir, stem)
  for _, ext in ipairs({ ".png", ".jpg", ".jpeg", ".bmp", ".gif" }) do if kmod.exists(dir .. "/" .. stem .. ext) then return true end end
end

-- ---- the square cropper (a UI.web page): the picture goes in, a 1:1 JPEG comes out
local cropOn = true      -- the import window's "crop to a square" switch
local B64 = {}
for i = 1, 64 do B64[("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"):byte(i)] = i - 1 end
local function b64decode(str)
  str = str:gsub("[^%w%+/]", "")
  local out, n, len = {}, 0, #str
  for i = 1, len, 4 do
    local a, b, c, d = str:byte(i, i + 3)
    local v = B64[a] * 262144 + B64[b] * 4096 + (c and B64[c] or 0) * 64 + (d and B64[d] or 0)
    n = n + 1
    if d then out[n] = string.char(v >> 16, (v >> 8) & 255, v & 255)
    elseif c then out[n] = string.char(v >> 16, (v >> 8) & 255)
    else out[n] = string.char(v >> 16) end
  end
  return table.concat(out)
end

local CROP_KEYS = { "crop_title", "crop_hint", "crop_zoom", "crop_confirm", "crop_cancel", "crop_working", "crop_sending", "crop_bad", "crop_big" }
local cropWin
-- done(jpegBytes) or done(nil) when the player cancels or the picture cannot be used
local function openCropper(srcPath, done, rectMode)
  if cropWin then pcall(function() cropWin:close() end); cropWin = nil end
  local phone = kmod.platform == "Android"
  local data = kmod.readFile(srcPath)
  local finished = false
  local w
  local function finish(bytes)
    if finished then return end
    finished = true
    cropWin = nil
    if w then pcall(function() w:close() end) end
    done(bytes)
  end
  local init = { cmd = "init", t = {}, rect = rectMode and true or nil }
  for _, k in ipairs(CROP_KEYS) do init.t[k] = Lang.t(k) end
  if not data then init.err = Lang.t("crop_bad")
  elseif #data > 40 * 1024 * 1024 then init.err = Lang.t("crop_big")
  else
    local ext = srcPath:match("%.([^.]+)$")
    ext = ext and ext:lower() or "png"
    local mime = (ext == "jpg" or ext == "jpeg") and "image/jpeg" or (ext == "bmp" and "image/bmp" or (ext == "gif" and "image/gif" or "image/png"))
    init.uri = "data:" .. mime .. ";base64," .. kmod.base64(data)
  end
  data = nil
  local parts = {}
  local ok, res = pcall(function()
    return UI.web{
      dir = "ui", entry = "crop.html", width = 760, height = phone and 790 or 860, design_width = 560,
      min_width = 760, max_width = 760, min_height = phone and 790 or 860, max_height = phone and 790 or 860,
      on_ready = function() w:send(init) end,
      on_message = function(m)
        if type(m) ~= "table" then return end
        if m.cmd == "chunk" and tonumber(m.i) and type(m.data) == "string" then
          parts[tonumber(m.i) + 1] = m.data
          w:send({ cmd = "ack", i = m.i })
        elseif m.cmd == "done" then
          local ok2, bytes = pcall(function() return b64decode(table.concat(parts)) end)
          if ok2 and #bytes > 100 then finish(bytes) else Log.warn("crop: bad data"); finish(nil) end
        elseif m.cmd == "rect" and tonumber(m.x) and tonumber(m.y) and tonumber(m.s) then
          finish({ x = math.floor(m.x), y = math.floor(m.y), s = math.max(8, math.floor(m.s)) })
        elseif m.cmd == "cancel" then finish(nil) end
      end,
      on_close = function() finish(nil) end,
    }
  end)
  if not ok then Log.warn("cropper unavailable: " .. tostring(res)); return false end
  w = res
  cropWin = w
  return true
end

-- the player picks an image; it becomes one more variant of `asset` inside the pack `packName` (created if it does not exist yet)
local function addVariant(packName, asset, base, det, w, done, animated)
  local dir = ART .. "/" .. packName
  local created = not kmod.exists(dir)
  kmod.mkdirs(dir)
  local list = readIndex(dir)
  local n = 1
  while stemTaken(dir, base .. "_" .. n) do n = n + 1 end
  local function abort(err)
    if created and not kmod.exists(dir .. "/index.json") then pcall(kmod.rmtree, dir) end
    if err and err ~= "cancelled" then toast(Lang.t("import_failed") .. ": " .. tostring(err)); Log.warn("import failed: " .. tostring(err)) end
    done(nil)
  end
  local function register(path, crop)
    local file = path:gsub("\\", "/"):match("([^/]+)$")
    list[#list + 1] = { Asset = asset, File = file, Crop = crop }
    kmod.writeFile(dir .. "/index.json", kmod.json.encode(list))
    scan()
    local key = packName
    for _, v in ipairs(byAsset[asset] or {}) do if v.pack == packName and v.file:find(file, 1, true) then key = v.key end end
    choices[asset] = key
    saveChoices()
    if w:IsValid() then apply(w) end
    applyAll3D()
    if det:IsValid() then refreshButton(det, asset, true) end
    Log.info("imported " .. path .. " as " .. key .. " for " .. asset)
    done(key)
  end
  if animated then                                                    -- a GIF: the file is kept as it is
    local started = Media.pick_file(dir .. "/" .. base .. "_" .. n, "gif", function(path, original)
      if not path then abort(original or "cancelled"); return end
      if cropOn and UI and UI.web and path:lower():match("%.gif$") then          -- a GIF can be cropped too: only the square is remembered, the file stays whole
        local opened = openCropper(path, function(rect)
          if not rect then pcall(os.remove, path); abort("cancelled"); return end
          register(path, rect)
        end, true)
        if not opened then register(path) end
        return
      end
      register(path)
    end)
    if not started then abort("unavailable") end
    return
  end
  local crop = cropOn and UI and UI.web
  local target = dir .. "/" .. base .. "_" .. n
  if crop then kmod.mkdirs(DATA .. "/crop"); target = DATA .. "/crop/source" end
  local started = Media.pick_image(target, function(path, err)
    if not path then abort(err or "cancelled"); return end
    if not crop then register(path); return end
    local opened = openCropper(path, function(bytes)
      pcall(os.remove, path)
      if not bytes then abort("cancelled"); return end
      local out = dir .. "/" .. base .. "_" .. n .. ".jpg"
      if not kmod.writeFile(out, bytes) then abort("could not write the picture"); return end
      register(out)
    end)
    if not opened then register(path) end        -- no cropper on this loader: use the picture as it is
  end)
  if not started then
    if created then pcall(kmod.rmtree, dir) end
    toast(Lang.t("import_failed"))
    done(nil)
  end
end

local importWin, openManager, openManagerWin, manageWin
local function openImportWindow(det, w, asset)
  if not (Media and UI) then return end
  if importWin then pcall(function() importWin:close() end); importWin = nil end
  local base = (asset:match("([^/]+)$") or "card"):gsub("[^%w_%-]", "")
  local NEW = Lang.t("new_pack")
  local target, newName = NEW, cleanName(Lang.t("imported") .. "-" .. base)
  local win
  local function build(status)
    win:clear()
    win:label(Lang.t("import_card") .. ": " .. base)
    local mine = byAsset[asset] and #byAsset[asset] or 0
    win:label(string.format(Lang.t("import_have"), mine))
    if UI and UI.web then win:toggle(Lang.t("crop_toggle"), cropOn, function(on) cropOn = on end) end
    local options = { NEW }
    for _, p in ipairs(packs) do options[#options + 1] = p end
    win:choice(Lang.t("import_to"), options, target, function(o) target = o end)
    local nameInput = win:input(Lang.t("pack_name"), newName, function(text) newName = cleanName(text) end)
    win:button(Lang.t("pick"), function()
      local pack = target
      if target == NEW then
        pack = cleanName(nameInput.get())
        if pack == "" then pack = cleanName(Lang.t("imported") .. "-" .. base) end
        newName = pack
      end
      addVariant(pack, asset, base, det, w, function(key)
        if not key then return end
        target = pack
        if importWin then build(Lang.t("import_done") .. ": " .. showName(key)) end
      end)
    end)
    if Media and Media.pick_file then
      win:button(Lang.t("import_anim"), function()
        local pack = target
        if target == NEW then
          pack = cleanName(nameInput.get())
          if pack == "" then pack = cleanName(Lang.t("imported") .. "-" .. base) end
          newName = pack
        end
        addVariant(pack, asset, base, det, w, function(key)
          if not key then return end
          target = pack
          if importWin then build(Lang.t("import_done") .. ": " .. showName(key)) end
        end, true)
      end)
    end
    if status then win:label(status) end
    win:spacer(8)
    win:button(Lang.t("mgr_title"), function() openManager() end)
  end
  win = UI.window{ title = Lang.t("import_title"), width = 720, on_close = function()
    importWin = nil
    pcall(function() win:close() end)                              -- destroy it: a hidden window would stay in memory
  end }
  importWin = win
  build()
end

-- ---------------------------------------------------------------- the art pack manager window
local function loadMeta()
  local txt = kmod.readFile(META)
  local m = txt and kmod.json.decode(txt)
  disabled = (type(m) == "table" and type(m.disabled) == "table") and m.disabled or {}
end
local function saveMeta() kmod.writeFile(META, kmod.json.encode({ disabled = disabled })) end

local function packOfKey(key) return key:match("^(.*)#%d+$") or key end

-- forget the choices that point into `pack` (the pack was switched off, renamed, shrunk or deleted): those cards show their own art again
local function dropChoices(pack)
  for asset, key in pairs(choices) do if packOfKey(key) == pack or key == pack then choices[asset] = nil end end
  saveChoices()
end

-- after any change: rescan and repaint every card that is on screen
local function refreshEverything()
  scan()
  for _, cn in ipairs(DETAIL) do
    for _, det in ipairs(FindAllOf(cn) or {}) do
      local w = cardOf(det)
      local ok, asset = pcall(function() return w and apply(w) end)
      refreshButton(det, ok and asset or nil)
    end
  end
  applyAll3D()
end

local function renamePack(old, new)
  new = cleanName(new)
  if new == "" or new == old then return false, Lang.t("mgr_bad_name") end
  if kmod.exists(ART .. "/" .. new) then return false, Lang.t("mgr_exists") end
  local ok, err = kmod.copytree(ART .. "/" .. old, ART .. "/" .. new)
  if not ok then pcall(kmod.rmtree, ART .. "/" .. new); return false, tostring(err) end
  pcall(kmod.rmtree, ART .. "/" .. old)
  for asset, key in pairs(choices) do                                 -- keep the choices that pointed into this pack
    if key == old then choices[asset] = new
    elseif packOfKey(key) == old then choices[asset] = new .. key:sub(#old + 1) end
  end
  saveChoices()
  if disabled[old] then disabled[old] = nil; disabled[new] = true; saveMeta() end
  return true
end

-- ---- sharing packs: a pack is a folder (index.json + pictures); exported it is a zip with the extension .artpack
local function safeImageName(f)
  f = tostring(f or "")
  if f == "" or f:find("[/\\]") or f:find("..", 1, true) or f:find("%c") then return false end
  local ext = f:match("%.([^.]+)$")
  ext = ext and ext:lower()
  return ext == "png" or ext == "jpg" or ext == "jpeg" or ext == "bmp" or ext == "gif"
end

local function uniquePackName(base)
  base = cleanName(base)
  if base == "" then base = cleanName(Lang.t("imported")) end
  local name, n = base, 1
  while kmod.exists(ART .. "/" .. name) do n = n + 1; name = base .. "-" .. n end
  return name
end

local function exportPack(name, done)
  local tmp = DATA .. "/export/" .. name .. ".artpack"
  kmod.mkdirs(DATA .. "/export")
  local ok, err = Media.zip(ART .. "/" .. name, tmp)
  if not ok then done(nil, tostring(err)); return end
  local started = Media.save_file(tmp, name .. ".artpack", function(chosen, err2)
    pcall(os.remove, tmp)
    done(chosen, err2)
  end)
  if not started then pcall(os.remove, tmp); done(nil, "unavailable") end
end

-- the player picks an .artpack (or a .zip with the same layout); it is checked and unpacked into a new pack
local function importPackFile(done)
  local base = DATA .. "/import"
  pcall(kmod.rmtree, base)
  kmod.mkdirs(base)
  local started = Media.pick_file(base .. "/pack", "artpack,zip", function(path, original)
    local tmp = base .. "/unpacked"
    local function finish(name, err)
      pcall(kmod.rmtree, base)
      done(name, err)
    end
    if not path then finish(nil, original); return end
    kmod.mkdirs(tmp)
    local ok, err = kmod.unzip(path, tmp)
    if not ok then finish(nil, tostring(err)); return end
    local idx = readIndex(tmp)
    local entries, files = {}, {}
    for _, it in ipairs(idx) do
      if type(it) == "table" and type(it.Asset) == "string" and safeImageName(it.File) and kmod.exists(tmp .. "/" .. it.File) then
        local c = type(it.Crop) == "table" and tonumber(it.Crop.x) and tonumber(it.Crop.y) and tonumber(it.Crop.s)
          and { x = math.floor(it.Crop.x), y = math.floor(it.Crop.y), s = math.floor(it.Crop.s) } or nil
        entries[#entries + 1] = { Asset = it.Asset, File = it.File, Crop = c }
        files[it.File] = true
      end
    end
    if #entries == 0 then finish(nil, Lang.t("mgr_invalid_pack")); return end
    local name = uniquePackName((tostring(original or "pack"):gsub("%.[^.]*$", "")))
    local dir = ART .. "/" .. name
    kmod.mkdirs(dir)
    for f in pairs(files) do
      local data = kmod.readFile(tmp .. "/" .. f)
      if not (data and kmod.writeFile(dir .. "/" .. f, data)) then pcall(kmod.rmtree, dir); finish(nil, "could not write " .. f); return end
    end
    kmod.writeFile(dir .. "/index.json", kmod.json.encode(entries))
    finish(name)
  end)
  if not started then pcall(kmod.rmtree, base); done(nil, "unavailable") end
end

local PAGE = 12

openManagerWin = function()
  if not UI then return end
  if manageWin then pcall(function() manageWin:close() end) end
  local win
  win = UI.window{ title = Lang.t("mgr_title"), width = 780, on_close = function()
    manageWin = nil
    pcall(function() win:close() end)
  end }
  manageWin = win
  local showList, showPack

  -- a red button that needs a second click: the first click only changes its text
  local function danger(text, again, fn)
    local armed = false
    local b
    b = win:button(text, function()
      if not armed then armed = true; b:set_text(again); return end
      fn()
    end)
  end

  function showList(status)
    win:clear()
    win:label(string.format(Lang.t("mgr_intro"), #packs))
    if Media and Media.pick_file then
      win:button(Lang.t("mgr_import_pack"), function()
        importPackFile(function(name, err)
          if not name then
            if err ~= "cancelled" then toast(Lang.t("mgr_failed") .. ": " .. tostring(err)); showList(Lang.t("mgr_failed") .. ": " .. tostring(err)) end
            return
          end
          refreshEverything()
          toast(Lang.t("mgr_imported") .. ": " .. name)
          showList(Lang.t("mgr_imported") .. ": " .. name)
        end)
      end)
    end
    if #packs == 0 then win:label(Lang.t("mgr_none")) end
    for _, name in ipairs(packs) do
      local info = packInfo[name] or { images = 0, cards = 0 }
      win:toggle(name .. "   " .. string.format(Lang.t("mgr_counts"), info.images, info.cards), not disabled[name], function(on)
        disabled[name] = (not on) or nil
        saveMeta()
        if not on then dropChoices(name) end
        refreshEverything()
      end)
      win:button(Lang.t("mgr_open") .. ": " .. name, function() showPack(name, 1) end)
      win:spacer(4)
    end
    if status then win:label(status) end
  end

  function showPack(name, page, status)
    win:clear()
    local info = packInfo[name]
    if not info then showList(); return end
    win:label(Lang.t("mgr_pack") .. ": " .. name .. "   " .. string.format(Lang.t("mgr_counts"), info.images, info.cards))
    win:toggle(Lang.t("mgr_enabled"), not disabled[name], function(on)
      disabled[name] = (not on) or nil
      saveMeta()
      if not on then dropChoices(name) end
      refreshEverything()
    end)

    local newName = name
    local nameInput = win:input(Lang.t("mgr_rename_to"), name, function(text) newName = cleanName(text) end)
    win:button(Lang.t("mgr_rename"), function()
      local ok, err = renamePack(name, nameInput.get())
      if not ok then showPack(name, page, Lang.t("mgr_failed") .. ": " .. tostring(err)); return end
      local to = cleanName(nameInput.get())
      refreshEverything()
      showPack(to, 1, Lang.t("mgr_renamed") .. ": " .. to)
    end)

    if Media and Media.save_file and Media.zip then
      win:button(Lang.t("mgr_export"), function()
        exportPack(name, function(chosen, err)
          if not chosen then
            if err ~= "cancelled" then toast(Lang.t("mgr_failed") .. ": " .. tostring(err)); showPack(name, page, Lang.t("mgr_failed") .. ": " .. tostring(err)) end
            return
          end
          toast(Lang.t("mgr_exported") .. ": " .. tostring(chosen))
          showPack(name, page, Lang.t("mgr_exported") .. ": " .. tostring(chosen))
        end)
      end)
    end

    win:spacer(6)
    local total = #info.entries
    local pages = math.max(1, math.ceil(total / PAGE))
    page = math.min(math.max(page or 1, 1), pages)
    win:label(string.format(Lang.t("mgr_images"), total, page, pages))
    for i = (page - 1) * PAGE + 1, math.min(total, page * PAGE) do
      local it = info.entries[i]
      local card = tostring(it.Asset):match("([^/]+)$") or "?"
      danger(Lang.t("mgr_remove") .. ": " .. card .. "  (" .. tostring(it.File) .. ")", Lang.t("mgr_again"), function()
        local dir = ART .. "/" .. name
        local list = readIndex(dir)
        local gone = table.remove(list, i)
        kmod.writeFile(dir .. "/index.json", kmod.json.encode(list))
        local used = false                                              -- delete the image file unless another entry still uses it
        for _, o in ipairs(list) do if o.File == (gone and gone.File) then used = true end end
        if gone and not used then pcall(os.remove, dir .. "/" .. gone.File) end
        dropChoices(name)                                               -- the numbering of the pack's variants may have shifted
        refreshEverything()
        showPack(name, page, Lang.t("mgr_removed"))
      end)
    end
    if pages > 1 then
      if page > 1 then win:button(Lang.t("mgr_prev"), function() showPack(name, page - 1) end) end
      if page < pages then win:button(Lang.t("mgr_next"), function() showPack(name, page + 1) end) end
    end

    win:spacer(8)
    danger(Lang.t("mgr_delete_pack"), Lang.t("mgr_again"), function()
      dropChoices(name)
      disabled[name] = nil
      saveMeta()
      pcall(kmod.rmtree, ART .. "/" .. name)
      refreshEverything()
      showList(Lang.t("mgr_deleted") .. ": " .. name)
    end)
    win:button(Lang.t("mgr_back"), function() showList() end)
    if status then win:label(status) end
  end

  showList()
end

-- ---------------------------------------------------------------- the same manager as a web page (UI.web), when the loader has it
local webMgr
local TKEYS = { "mgr_title", "mgr_intro", "mgr_none", "mgr_counts", "mgr_open", "mgr_rename_to", "mgr_rename", "mgr_images", "mgr_remove", "mgr_again",
  "mgr_prev", "mgr_next", "mgr_delete_pack", "mgr_back", "mgr_import_pack", "mgr_export", "mgr_search", "mgr_preview", "mgr_hide", "mgr_too_big",
  "mgr_no_match", "mgr_match_packs", "mgr_match_files", "mgr_art_new", "mgr_anim" }

local function webState()
  local list = {}
  for _, name in ipairs(packs) do
    local info = packInfo[name] or { entries = {}, images = 0, cards = 0 }
    local es = {}
    local lang = Lang.current()
    for _, it in ipairs(info.entries) do
      local e = names[tostring(it.Asset):lower()]
      es[#es + 1] = { card = tostring(it.Asset):match("([^/]+)$") or "?", name = e and e[lang] or nil, file = tostring(it.File) }
    end
    list[#list + 1] = { name = name, enabled = not disabled[name], images = info.images, cards = info.cards, entries = es }
  end
  return list
end

local function openWebManager()
  if webMgr and webMgr:is_open() then return true end
  if webMgr then pcall(function() webMgr:close() end); webMgr = nil end
  local w
  local phone = kmod.platform == "Android"
  local function push(text, ok, extra)
    local m = { cmd = "state", packs = webState(), status = text, ok = ok ~= false }
    for k, v in pairs(extra or {}) do m[k] = v end
    w:send(m)
  end
  local function fail(err) toast(Lang.t("mgr_failed") .. ": " .. tostring(err)); push(Lang.t("mgr_failed") .. ": " .. tostring(err), false) end
  local actions = {
    toggle = function(m)
      if not packInfo[m.pack] then return end
      local on = disabled[m.pack] and true or false
      disabled[m.pack] = (not on) or nil
      saveMeta()
      if not on then dropChoices(m.pack) end
      refreshEverything()
      push()
    end,
    rename = function(m)
      local to = cleanName(m.to)
      local ok, err = renamePack(m.pack, to)
      if not ok then fail(err); return end
      refreshEverything()
      push(Lang.t("mgr_renamed") .. ": " .. to, true, { view = to })
    end,
    remove = function(m)
      local dir = ART .. "/" .. tostring(m.pack)
      local list = readIndex(dir)
      local i = tonumber(m.i)
      if not (packInfo[m.pack] and i and list[i]) then return end
      local gone = table.remove(list, i)
      kmod.writeFile(dir .. "/index.json", kmod.json.encode(list))
      local used = false                                              -- delete the image file unless another entry still uses it
      for _, o in ipairs(list) do if o.File == gone.File then used = true end end
      if not used and safeImageName(gone.File) then pcall(os.remove, dir .. "/" .. gone.File) end
      dropChoices(m.pack)                                             -- the numbering of the pack's variants may have shifted
      refreshEverything()
      push(Lang.t("mgr_removed"))
    end,
    delete = function(m)
      if not packInfo[m.pack] then return end
      dropChoices(m.pack)
      disabled[m.pack] = nil
      saveMeta()
      pcall(kmod.rmtree, ART .. "/" .. m.pack)
      refreshEverything()
      push(Lang.t("mgr_deleted") .. ": " .. m.pack, true, { gone = true })
    end,
    export = function(m)
      if not packInfo[m.pack] then return end
      exportPack(m.pack, function(chosen, err)
        if not chosen then if err ~= "cancelled" then fail(err) end return end
        toast(Lang.t("mgr_exported") .. ": " .. tostring(chosen))
        push(Lang.t("mgr_exported") .. ": " .. tostring(chosen))
      end)
    end,
    preview = function(m)
      local info = packInfo[m.pack]
      local it = info and info.entries[tonumber(m.i) or 0]
      if not (it and safeImageName(it.File)) then return end
      local pext = it.File:match("%.([^.]+)$"):lower()
      if pext == "gif" then w:send({ cmd = "preview", pack = m.pack, i = m.i, anim = true }); return end
      local data = kmod.readFile(ART .. "/" .. m.pack .. "/" .. it.File)
      if not data then return end
      if #data > 20 * 1024 * 1024 then w:send({ cmd = "preview", pack = m.pack, i = m.i, err = true }); return end
      local ext = it.File:match("%.([^.]+)$"):lower()
      local mime = (ext == "jpg" or ext == "jpeg") and "image/jpeg" or (ext == "bmp" and "image/bmp" or "image/png")
      w:send({ cmd = "preview", pack = m.pack, i = m.i, uri = "data:" .. mime .. ";base64," .. kmod.base64(data) })
    end,
    import = function()
      importPackFile(function(name, err)
        if not name then if err ~= "cancelled" then fail(err) end return end
        refreshEverything()
        toast(Lang.t("mgr_imported") .. ": " .. name)
        push(Lang.t("mgr_imported") .. ": " .. name)
      end)
    end,
  }
  local ok, res = pcall(function()
    return UI.web{
      dir = "ui", width = phone and 1180 or 1100, height = phone and 760 or 800, design_width = phone and 700 or 600,    -- on the phone the page is drawn larger
      min_width = phone and 700 or 600, min_height = phone and 480 or 460, max_width = 2000, max_height = 1200,
      on_ready = function()
        local t = {}
        for _, k in ipairs(TKEYS) do t[k] = Lang.t(k) end
        w:send({ cmd = "init", t = t, packs = webState() })
      end,
      on_message = function(m)
        local f = type(m) == "table" and actions[m.cmd]
        if f then
          local ok2, err = pcall(f, m)
          if not ok2 then Log.warn("manager action " .. tostring(m.cmd) .. " failed: " .. tostring(err)); fail(err) end
        end
      end,
      on_close = function() webMgr = nil end,
    }
  end)
  if not ok then Log.warn("web manager unavailable: " .. tostring(res)); return false end
  w = res
  webMgr = w
  return true
end

openManager = function()
  if UI and UI.web and openWebManager() then return end
  openManagerWin()
end

local function detailAt(addr)
  for _, cn in ipairs(DETAIL) do
    for _, d in ipairs(FindAllOf(cn) or {}) do if d:GetAddress() == addr then return d end end
  end
end

RegisterHook(CLICK, function() end, function(self)
  for _, mb in pairs(manageButtons) do
    if mb:IsValid() and mb == self then openManager(); return end
  end
  for addr, ib in pairs(importButtons) do
    if ib:IsValid() and ib == self then
      local det = detailAt(addr)
      local w = det and cardOf(det)
      local st = w and state[w.mat:GetAddress()]
      if st then openImportWindow(det, w, st.asset) end
      return
    end
  end
  for addr, b in pairs(buttons) do
    if b:IsValid() and b == self then
      local det
      for _, cn in ipairs(DETAIL) do
        for _, d in ipairs(FindAllOf(cn) or {}) do if d:GetAddress() == addr then det = d end end
      end
      if not det then return end
      local w = cardOf(det)
      if not w then return end
      local st = state[w.mat:GetAddress()]
      if not st then return end
      local list = variants(st.asset)
      local cur = choices[st.asset] or ""
      local idx = 1
      for i, v in ipairs(list) do if v == cur then idx = i end end
      local nextv = list[(idx % #list) + 1]
      choices[st.asset] = (nextv ~= "") and nextv or nil
      saveChoices()
      apply(w)
      applyAll3D()
      refreshButton(det, st.asset)
      Log.info(st.asset .. " -> " .. ((nextv ~= "") and nextv or "original"))
    end
  end
end)

loadMeta()
scan()
loadChoices()
do local t = kmod.readFile(NAMES); t = t and kmod.json.decode(t); if type(t) == "table" then names = t end end
-- The game sets a card's art inside its own graphs, which no hook can see, so we poll:
--   * every ~8 ms (FastLoop): the cards we already know have variants -- fixes a flash of the original art within a frame
--   * every second: scan all card objects for new ones
--   * when a card object is created: scan again right away (and 150 ms later)
FastLoop(8, function()                        -- runs every few ms on the game thread (core 'fast lane'), not only at 20 Hz
  for addr, t in pairs(tracked) do
    if not t.mat:IsValid() then tracked[addr] = nil
    else
      local ok, err = pcall(applyMat, t.mat, t.refresh, nil, t.param)
      if not ok then Log.warn("fast check: " .. tostring(err)); tracked[addr] = nil end
    end
  end
  -- objects created a moment ago: their material appears some hundreds of ms later (the game builds the card in its own
  -- graph), so look at each of them every 50 ms until it has one, for at most 3 s
  for i = #pending, 1, -1 do
    local p = pending[i]
    p.age = p.age + 1
    local done = true
    if p.obj:IsValid() and p.age <= 60 then
      local ok, asset = pcall(p.fn, p.obj)
      done = ok and asset ~= nil
    end
    if done then table.remove(pending, i) end
  end
  return false
end)
LoopAsync(1000, function() applyAll3D(); return false end)

local function watch(fn) return function(obj) if next(byAsset) ~= nil then pending[#pending + 1] = { obj = obj, fn = fn, age = 0 } end end end
for _, cn in ipairs(CARD3D) do NotifyOnNewObject(cn, watch(apply3D), true) end        -- true = fast scan (~60 Hz)
NotifyOnNewObject("W_CollectionCardDeckWidget_C", watch(applyRow), true)
NotifyOnNewObject("W_Card2D_C", watch(apply), true)

-- the detail page may already be open when the mod loads
for _, w in ipairs(FindAllOf("W_Card2D_C") or {}) do
  local det = detailOf(w)
  if det then
    local ok, vis = pcall(function() return w:IsVisible() end)
    if ok and vis then local asset = apply(w); ensureButton(det); refreshButton(det, asset, true) end
  end
end
