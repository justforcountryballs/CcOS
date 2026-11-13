"""Copy the firmware ccos's drivers ask for into rootfs/lib/firmware (kept .zst: the kernel unpacks it).
   python3 tools/pick_firmware.py /lib/firmware rootfs/lib/firmware"""
import os, re, glob, shutil, sys
src, dst = sys.argv[1], sys.argv[2]
os.chdir(src)
pick = set()
def add(pats):
    for p in pats:
        for f in glob.glob(p, recursive=True):
            if os.path.isfile(f) or os.path.islink(f): pick.add(f)
# Intel Wi-Fi: the newest version for each chip from the last few years
iw = {}
for f in glob.glob('iwlwifi-*'):
    if not re.match(r'iwlwifi-(so|ty|ma|gl|bz|sc|Qu|QuZ|cc|9000|9260|8265|8000C|7265D|3168)', f): continue
    m = re.match(r'(iwlwifi-.*?)-(\d+)\.ucode', f)
    if not m: pick.add(f); continue                       # .pnvm
    k, v = m.group(1), int(m.group(2))
    if k not in iw or v > iw[k][0]: iw[k] = (v, f)
pick |= {v[1] for v in iw.values()}
add(['intel/ibt-0040-*', 'intel/ibt-0041-*', 'intel/ibt-0180-*', 'intel/ibt-0190-*', 'intel/ibt-0291-*', 'intel/ibt-1040-*',
     'intel/ibt-0093-*', 'intel/ibt-19-*', 'intel/ibt-20-*', 'intel/ibt-17-*', 'intel/ibt-18-*', 'intel/ibt-12-16*', 'intel/ibt-11-5*'])
add(['intel/sof/**', 'intel/sof-tplg/**', 'intel/sof-ipc4/**', 'intel/sof-ipc4-tplg/**', 'intel/sof-ipc4-lib/**', 'intel/sof-ace-tplg/**'])
add(['i915/*', 'rtw88/*', 'rtw89/*', 'rtlwifi/*', 'rtl_bt/*', 'rtl_nic/*', 'qca/*', 'radeon/*', 'regulatory.db*'])
add(['mediatek/WIFI_*MT79*', 'mediatek/BT_RAM_CODE_MT79*', 'mediatek/mt7921*', 'mediatek/mt7922*', 'mediatek/mt7925*', 'mediatek/mt7961*', 'mediatek/mt7601u.bin', 'mediatek/mt7662*', 'mediatek/mt7668*'])
add(['ath11k/WCN6855/**', 'ath11k/QCA6390/**', 'ath10k/QCA6174/**', 'ath10k/QCA9377/**', 'ath9k_htc/*', 'ath3k-1.fw'])
add(['brcm/brcmfmac43*-pcie*', 'brcm/brcmfmac4356*', 'brcm/brcmfmac4350*', 'brcm/BCM*'])
for f in glob.glob('amdgpu/*'):
    if re.search(r'(raven|picasso|renoir|green_sardine|yellow_carp|vangogh|dcn_3_1|dcn_3_5|gc_10_3_[67]|gc_11_0_[14]|gc_11_5|psp_1[234]|sdma_5_2_[67]|sdma_6_[01]|vcn_[34]|smu_1[34])', f): pick.add(f)
total = 0
for f in sorted(pick):
    out = os.path.join(dst, f)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if not os.path.exists(f): continue                    # a link to something not there
    if os.path.lexists(out): os.remove(out)
    shutil.copy2(f, out); total += os.path.getsize(f)     # links: a copy of what they point at
print(len(pick), 'files', round(total / 1e6, 1), 'MB')
