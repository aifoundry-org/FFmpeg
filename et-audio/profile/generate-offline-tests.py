#!/usr/bin/env python3
"""Generate ABI-3 coverage from the pinned offline native test suite."""
import hashlib
from pathlib import Path
import sys
PINNED_SHA256="e8ac3861e857f9b6b594c4e222c551afbb9d65ec783cdd8ccd6eb601fd0acd6c"
def main():
    if len(sys.argv)!=3: raise SystemExit("usage: generate-offline-tests.py BASE OUTPUT")
    src,out=map(Path,sys.argv[1:]); raw=src.read_bytes(); got=hashlib.sha256(raw).hexdigest()
    if got!=PINNED_SHA256: raise ValueError(f"refusing offline test drift: expected {PINNED_SHA256}, got {got}")
    s=raw.decode()
    for old,new in [('ETAACOfflineParams','ETAACProfileParams'),('ETAACStatus','ETAACProfileStatus'),
                    ('etaac_offline_','etaac_profile_'),('ETAAC_OFFLINE_','ETAAC_PROFILE_')]: s=s.replace(old,new)
    s=s.replace('b.status[i].samples','ETAAC_STATUS_SAMPLES(b.status[i].operation_samples)')
    s=s.replace('b.status[0].samples','ETAAC_STATUS_SAMPLES(b.status[0].operation_samples)')
    s=s.replace('sizeof(ETAACProfileStatus)', 'sizeof(ETAACProfileStatus)')
    if 'ETAACOfflineParams' in s or 'etaac_offline_' in s or '.samples' in s: raise ValueError('incomplete ABI-3 test adaptation')
    out.parent.mkdir(parents=True,exist_ok=True); out.write_text(s)
if __name__=='__main__': main()
