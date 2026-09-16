--[[
  doom_launcher.lua — Luac0re payload for doom-ps
  Based on EmuC0re's nes.lua (EgyDevTeam).

  EDIT THESE FOUR LINES before deploying:
--]]
local PC_IP    = "192.168.1.100"   -- your PC's IP (for UDP log)
local LOG_PORT = 9027              -- UDP port on your PC (log_server.py)
local WAD_PORT = 5000              -- TCP port Python sender connects to

-- Paste the output of `make hex` here after building.
local shellcode_hex = ""

-- ============================================================
-- Bootstrap (identical to nes.lua preamble)
-- ============================================================
init_dlsym()
sceMsgDialogTerminate()

-- ---- Log socket (UDP, non-blocking, mirrors nes.lua) ----
local function htons(p) return ((p << 8) | (p >> 8)) & 0xFFFF end
local function inet_addr(s)
    local a,b,c,d = s:match("(%d+)%.(%d+)%.(%d+)%.(%d+)")
    return (d << 24) | (c << 16) | (b << 8) | a
end
local function make_sockaddr_in(port, ip)
    local sa = malloc(16)
    for i = 0,15 do write8(sa + i, 0) end
    write8(sa + 0, 16); write8(sa + 1, 2)
    write16(sa + 2, htons(port))
    if ip then write32(sa + 4, inet_addr(ip)) end
    return sa
end

local log_sock = create_socket(AF_INET, SOCK_DGRAM, 0)
local log_sa   = make_sockaddr_in(LOG_PORT, PC_IP)

local function ulog(m)
    if log_sock >= 0 then
        syscall.sendto(log_sock, m .. "\n", #m + 1, 0, log_sa, 16)
    end
end
ulog("doom_launcher: starting")

-- ---- userId (same as nes.lua) ----
local libUser  = sceKernelLoadStartModule("libSceUserService.sprx", 0,0,0,0,0)
local getUserId = dlsym(libUser, "sceUserServiceGetInitialUser")
local uid_buf  = malloc(4)
write32(uid_buf, 0)
if getUserId then func_wrap(getUserId)(uid_buf) end
local userId = read32(uid_buf)
ulog("userId = " .. tostring(userId))

-- ---- TCP listener for WAD upload ----
local tcp_srv = create_socket(AF_INET, SOCK_STREAM, 0)
local en = malloc(4); write32(en, 1)
syscall.setsockopt(tcp_srv, 0xFFFF, 0x0004, en, 4)  -- SO_REUSEADDR
local ba = make_sockaddr_in(WAD_PORT)
syscall.bind(tcp_srv, ba, 16)
syscall.listen(tcp_srv, 4)
ulog("doom_launcher: WAD TCP on port " .. tostring(WAD_PORT))

-- ---- Build ext_args (mirrors struct ext_args in core.h exactly) ----
--
--   Offset  Field
--   0x00    s64 status        (written by shellcode on exit)
--   0x08    s64 step          (progress marker)
--   0x10    u32 frame_count
--   0x14    s32 log_fd
--   0x18    u8  log_addr[16]  (sockaddr_in for UDP log)
--   0x28    u64 dbg[0]        tcp_srv fd
--   0x30    u64 dbg[1]        WAD_PORT (informational)
--   0x38    u64 dbg[2]        userId
--
local ext = malloc(0x80)
memset(ext, 0, 0x80)
write64(ext + 0x00, 0xDEAD)
write32(ext + 0x14, log_sock)
for i = 0, 15 do write8(ext + 0x18 + i, read8(log_sa + i)) end
write64(ext + 0x28, tcp_srv)
write64(ext + 0x30, WAD_PORT)
write64(ext + 0x38, userId)

-- ---- Write and execute shellcode ----
if shellcode_hex ~= "" then
    local sc  = hex_to_binary(shellcode_hex)
    write_shellcode(SHELLCODE_BASE, sc)
    ulog("doom_launcher: shellcode written (" .. tostring(#sc) .. " bytes)")
    func_wrap(SHELLCODE_BASE)(EBOOT_BASE, SCE_KERNEL_DLSYM, ext)
else
    ulog("doom_launcher: shellcode_hex is empty — run 'make hex' and paste here")
end

-- ---- Optional monitor loop ----
local prev_fc = 0
for _ = 1, 6000 do
    local fc   = read32(ext + 0x10)
    local step = read64(ext + 0x08)
    if fc ~= prev_fc then
        if fc % 300 == 0 then ulog("doom_launcher: frame=" .. tostring(fc)) end
        prev_fc = fc
    end
    if step == 99 then
        ulog("doom_launcher: shellcode exited cleanly")
        break
    end
    sceKernelUsleep(500000)
end
ulog("doom_launcher: done")
