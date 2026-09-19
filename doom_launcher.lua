--[[
  doom_launcher.lua — Luac0re payload for doom-ps
--]]
local PC_IP        = "" -- PLZ..... put your ip here not the PS4/PS5 ip! .. examples.. (192.168.x.x)
local LOG_PORT     = 9027
local WAD_PORT     = 5000
local SC_PORT_BASE = 5001
local SC_PORT_MAX  = 5020

init_dlsym()
sceMsgDialogTerminate()

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
ulog("doom_launcher: starting v10")

-- ============================================================
-- Memory
-- ============================================================
local SC_TARGET = 0x100000
local SC_MIN    = 0x9A000
local rw, rx    = 0, 0
local SC_SIZE   = SC_TARGET

-- ---- mmap PRIMARY ----
do
    local PROT_RWX      = 0x7
    local MAP_PRIV_ANON = 0x1002
    local m = syscall.mmap(0, SC_TARGET, PROT_RWX, MAP_PRIV_ANON, -1, 0)
    ulog("mmap RWX 1MB -> 0x" .. string.format("%x", m))
    if m and m > 0x10000 then           -- any valid user address
        rw = m; rx = m
        ulog("PRIMARY mmap RWX at 0x" .. string.format("%x", m))
    end
end

-- ---- JIT fallback ----
if rw == 0 then
    local function jit_alloc(size)
        local bfd  = jit_malloc(8)
        local rwfd = jit_malloc(8)
        local rxfd = jit_malloc(8)
        local rwa  = jit_malloc(8)
        local rxa  = malloc(8)
        local nm   = jit_malloc(8)
        if bfd == 0 or rwfd == 0 or rxfd == 0 or rwa == 0 or nm == 0 then
            return 0, 0
        end
        jit_write_buffer(nm, "nv4b")
        jit_sceKernelJitCreateSharedMemory(nm, size, 7, bfd)
        local handle = jit_read32(bfd)
        if handle == 0 then return 0, 0 end
        jit_sceKernelJitCreateAliasOfSharedMemory(handle, PROT_READ|PROT_WRITE, rwfd)
        jit_sceKernelJitCreateAliasOfSharedMemory(handle, PROT_READ|PROT_EXECUTE, rxfd)
        jit_sceKernelJitMapSharedMemory(jit_read32(rwfd), PROT_READ|PROT_WRITE, rwa)
        local rw_try = jit_read64(rwa)
        if rw_try == 0 then return 0, 0 end
        local mfd = jit_send_recv_fd(jit_read32(rxfd), NEW_JIT_SOCK, NEW_MAIN_SOCK)
        sceKernelJitMapSharedMemory(mfd, PROT_READ|PROT_EXECUTE, rxa)
        local rx_try = read64(rxa)
        if rx_try == 0 then return 0, 0 end
        return rw_try, rx_try
    end

    local JIT_SIZES = {0x100000, 0xC0000, 0x80000}
    for _, size in ipairs(JIT_SIZES) do
        ulog("JIT try 0x" .. string.format("%x", size))
        local r, x = jit_alloc(size)
        if r ~= 0 then
            rw, rx = r, x
            SC_SIZE = size
            ulog("JIT ok at 0x" .. string.format("%x", r) ..
                 " size 0x" .. string.format("%x", size))
            break
        end
    end
end

if rw == 0 then
    ulog("FATAL: no RWX memory >= 630 KB")
    error("shellcode memory allocation failed (need 630 KB)")
end

ulog("shellcode dest rw=0x" .. string.format("%x", rw) ..
     " rx=0x" .. string.format("%x", rx) ..
     " size=0x" .. string.format("%x", SC_SIZE))

-- ============================================================
-- userId + WAD TCP
-- ============================================================
local userId = 0
ulog("userId = 0 (hardcoded)")

local tcp_srv = create_socket(AF_INET, SOCK_STREAM, 0)
local en = malloc(4); write32(en, 1)
syscall.setsockopt(tcp_srv, 0xFFFF, 0x0004, en, 4)
local ba = make_sockaddr_in(WAD_PORT)
syscall.bind(tcp_srv, ba, 16)
syscall.listen(tcp_srv, 4)
ulog("WAD TCP on port " .. tostring(WAD_PORT))

-- ============================================================
-- Free port scan
-- ============================================================
local srv, sc_port = -1, 0
for p = SC_PORT_BASE, SC_PORT_MAX do
    local s = create_socket(AF_INET, SOCK_STREAM, 0)
    if s >= 0 then
        local reuse = malloc(4); write32(reuse, 1)
        syscall.setsockopt(s, 0xFFFF, 0x0004, reuse, 4)
        local sa2 = make_sockaddr_in(p)
        if syscall.bind(s, sa2, 16) == 0 and syscall.listen(s, 1) == 0 then
            srv, sc_port = s, p
            break
        end
        syscall.close(s)
    end
end
if srv < 0 then
    ulog("FATAL: no free shellcode port in 5001..5020")
    error("shellcode port allocation failed")
end
ulog("SCPORT " .. tostring(sc_port))

-- ============================================================
-- Receive shellcode
-- ============================================================
local function receive_shellcode(dest, srv_fd, max_size)
    local sa = make_sockaddr_in(sc_port)
    local alen = malloc(8); write32(alen, 16)
    ulog("awaiting shellcode on port " .. tostring(sc_port))
    local cfd = syscall.accept(srv_fd, sa, alen)
    if cfd < 0 then syscall.close(srv_fd); error("accept failed") end

    local total, err_msg = 0, nil
    while total < max_size do
        local n = syscall.read(cfd, dest + total, max_size - total)
        if n == 0 then break end
        if n < 0 then
            err_msg = "read error " .. tostring(n) ..
                      " at offset " .. tostring(total)
            break
        end
        total = total + n
    end
    syscall.close(cfd)
    syscall.close(srv_fd)
    if err_msg then error(err_msg) end
    ulog("shellcode received " .. total .. " / " .. max_size .. " bytes")
    return total
end

local n = receive_shellcode(rw, srv, SC_SIZE)
if n < 0x90000 then
    error("short receive: got " .. tostring(n) .. " bytes, expected ~623 KB")
end

-- ============================================================
-- ext_args
-- ============================================================
local ext = malloc(0x80)
memset(ext, 0, 0x80)
write64(ext + 0x00, 0xDEAD)
write32(ext + 0x14, log_sock)
for i = 0, 15 do write8(ext + 0x18 + i, read8(log_sa + i)) end
write64(ext + 0x28, tcp_srv)
write64(ext + 0x30, WAD_PORT)
write64(ext + 0x38, userId)

-- ============================================================
-- Jump
-- ============================================================
ulog("entering shellcode at 0x" .. string.format("%x", rx))
func_wrap(rx)(EBOOT_BASE, SCE_KERNEL_DLSYM, ext)

local frames = read32(ext + 0x10)
ulog("done, frames=" .. tostring(frames))
