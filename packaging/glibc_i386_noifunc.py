# Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE)
# For packaging/setup-32bit-toolchain.sh: glibc's sysdeps/i386/dl-tls-get-addr.c picks ___tls_get_addr with an ifunc,
# which the musl gcc cannot compile. The 32-bit glibc there is only linked against (never run), so ___tls_get_addr
# simply calls the fnsave variant (defined after rtld_hidden_proto, so hidden_def finds it).
#   python3 glibc_i386_noifunc.py <path to dl-tls-get-addr.c>
import sys

p = sys.argv[1]
s = open(p).read()
ifunc = """libc_ifunc_redirected (__redirect____tls_get_addr, ___tls_get_addr,
\t\t       IFUNC_SELECTOR ());"""
proto = "rtld_hidden_proto (___tls_get_addr)\n"
body = """extern __typeof (__redirect____tls_get_addr) ___tls_get_addr;
""" + proto + """void *
___tls_get_addr (tls_index *ti)
{
  return OPTIMIZE (fnsave) (ti);
}
"""
if "return OPTIMIZE (fnsave) (ti);" in s:
    print("already patched", p)
elif ifunc in s and proto in s:
    s = s.replace(ifunc, "/* (Q-LiM: no ifunc, see below) */", 1).replace(proto, body, 1)
    open(p, "w").write(s)
    print("patched", p)
else:
    sys.exit("dl-tls-get-addr.c: the ifunc line was not found (another glibc version?)")
