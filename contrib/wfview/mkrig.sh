#!/usr/bin/env bash
#
# mkrig.sh - make the wfview rig file for the X6100 (x6100_trx) from wfview's IC-705 one
#
# Usage: mkrig.sh <wfview>/rigs/IC-705.rig > X6100.rig
#
# Keeps the commands x6100_trx answers (the rest would only give NG rounds), sets the
# X6100 identity (CI-V address 0x61, hamlib model 3087), our spans, bands, modes and
# memory layout. See README.md next to this script.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

set -euo pipefail

readonly C_CIV_ADDR=97		# 0x61: odd, so never a real Icom; matches the 1D 19 ID 61 00
readonly C_HAMLIB=3087		# RIG_MODEL_X6100

#
# Print usage and exit
#

usage() {
	printf '%s\n' "usage: mkrig.sh <wfview>/rigs/IC-705.rig > X6100.rig" >&2
	exit 1
}

if [[ $# -ne 1 || ! -r "${1}" ]]; then
	usage
fi

G_SRC="${1}"

awk -v CIV="${C_CIV_ADDR}" -v HAMLIB="${C_HAMLIB}" -f <(cat <<'EOF'
BEGIN {
	# Commands x6100_trx answers (wfview names, IC-705 file)
	n = split("Freq (TRX)|Mode (TRX)|Freq Get|Mode Get|Freq Set|Mode Set|" \
		"VFO Mode Select|VFO A Select|VFO B Select|VFO Equal AB|VFO Swap A/B|" \
		"Memory Mode|Memory Write|Memory to VFO|Memory Clear|Split/Duplex|Tuning Step|" \
		"Attenuator Status|AF Gain|RF Gain|Squelch|NR Level|CW Pitch|RF Power|Mic Gain|" \
		"Key Speed|Compressor Level|Break-In Delay|NB Level|Monitor Gain|Vox Gain|" \
		"Anti-Vox Gain|S Meter Sql Status|S Meter|Various Squelch|Overflow Status|" \
		"Power Meter|SWR Meter|ALC Meter|Vd Meter|Preamp Status|AGC Time Constant|" \
		"Noise Blanker|Noise Reduction|Compressor Status|Monitor Status|Vox Status|" \
		"Manual Notch|Dial Lock Status|Transceiver ID|Memory Contents|Band Stacking Reg|" \
		"Filter Width|Quick Split|CIV Transceive|Data Off Mod Input|DATA1 Mod Input|" \
		"Data Mode Filter|Transceiver Status|Tuner/ATU Status|TX Freq|RIT Frequency|" \
		"RIT Status|RIT TX Status|Selected Freq|Unselected Freq|Selected Mode|" \
		"Unselected Mode|Scope Wave Data|Scope On/Off|Scope Data Output|Scope Main/Sub|" \
		"Scope Single/Dual|Scope Mode|Scope Span|Scope Edge|Scope Hold|Scope Ref|" \
		"Scope Speed|Scope VBW|Scope Fixed Edge Freq|Command Error FA|Command OK FB", a, "|")
	for (i = 1; i <= n; i++)
		want["Commands", a[i]] = 1
	# Periodic polls: only for commands we keep (checked against want[] below)
	n = split("6m|10m|12m|15m|17m|20m|30m|40m|60m|80m|160m|Gen", a, "|")
	for (i = 1; i <= n; i++)
		want["Bands", a[i]] = 1
	n = split("LSB|USB|AM|CW|FM|CW-R", a, "|")
	for (i = 1; i <= n; i++)
		want["Modes", a[i]] = 1
	want["Inputs", "MIC"] = 1
	want["Inputs", "USB"] = 1
	want["Preamps", "0"] = 1
	want["Preamps", "1"] = 1
	n = split("ALC|Center|Power|S-Meter|SWR|Voltage", a, "|")
	for (i = 1; i <= n; i++)
		want["Meters", a[i]] = 1
	# The record field that names it
	keyf["Commands"] = "Type"
	keyf["Periodic"] = "Command"
	keyf["Bands"] = "Name"
	keyf["Modes"] = "Name"
	keyf["Inputs"] = "Name"
	keyf["Preamps"] = "Num"
	keyf["Meters"] = "Meter"
	keyf["CTCSS"] = "-"		# no tone encoder: none kept
	keyf["DTCS"] = "-"
	keyf["Spans"] = "-"		# replaced by ours
	# Field changes: section SUBSEP name SUBSEP field
	set["Commands", "CIV Transceive", "String"] = "\\\\x1a\\\\x05\\\\x01\\\\x31"
	set["Commands", "Preamp Status", "Max"] = "1"
	set["Commands", "Scope Span", "Max"] = "5"
	set["Commands", "Tuning Step", "Max"] = "13"
	set["Bands", "Gen", "Start"] = "500000"
	set["Bands", "Gen", "End"] = "54000000"
	set["Modes", "AM", "Max"] = "6000"
	set["Preamps", "1", "Name"] = "PREAMP"
	# Header
	hdr["Model"] = "X6100"
	hdr["CIVAddress"] = CIV
	hdr["RigCtlDModel"] = HAMLIB
	hdr["HasLAN"] = "false"
	hdr["HasWiFi"] = "false"
	hdr["MemGroups"] = "0"
	hdr["Memories"] = "99"
	hdr["MemStart"] = "1"
	# 1A 00 as x6100_trx sends it: the IC-705 A side only, the name right after R2
	hdr["MemFormat"] = "%1.2a %3.2b %5.1d %6.5f %11.1g %12.1h %13.1i %14.1j %15.1m %16.3n " \
		"%19.3o %22.1p %23.2q %25.1r %26.3s %29.8t %37.8u %45.8v %53.16z"
}

# Record line: Section\N\Field=value
function rec(line, r,   p, q) {
	if (!match(line, /^[A-Za-z%0-9]+\\[0-9]+\\[A-Za-z0-9]+=/))
		return 0
	p = index(line, "\\")
	r["sec"] = substr(line, 1, p - 1)
	q = substr(line, p + 1)
	r["idx"] = substr(q, 1, index(q, "\\") - 1) + 0
	q = substr(q, index(q, "\\") + 1)
	r["fld"] = substr(q, 1, index(q, "=") - 1)
	r["val"] = substr(q, index(q, "=") + 1)
	return 1
}

# Pass 1: the name of every record
FNR == NR {
	if (rec($0, r)) {
		if ((r["sec"] in keyf) && r["fld"] == keyf[r["sec"]])
			name[r["sec"], r["idx"]] = r["val"]
		if (r["idx"] > last[r["sec"]])
			last[r["sec"]] = r["idx"]
	}
	next
}

# Between the passes: what to keep, new numbers
FNR == 1 {
	for (s in last) {
		cnt[s] = 0
		for (i = 1; i <= last[s]; i++) {
			k = (s, i) in name ? name[s, i] : ""
			if (!(s in keyf))
				ok = 1
			else if (s == "Periodic")
				ok = ("Commands", k) in want
			else
				ok = (s, k) in want
			if (ok)
				newidx[s, i] = ++cnt[s]
		}
	}
	cnt["Commands"]++		# Notch Filter, added below
}

{
	if (rec($0, r)) {
		s = r["sec"]
		if (!((s, r["idx"]) in newidx))
			next
		v = r["val"]
		k = name[s, r["idx"]]
		if ((s, k, r["fld"]) in set)
			v = set[s, k, r["fld"]]
		printf "%s\\%d\\%s=%s\n", s, newidx[s, r["idx"]], r["fld"], v
		next
	}
	if (match($0, /^[A-Za-z%0-9]+\\size=/)) {
		s = substr($0, 1, index($0, "\\") - 1)
		if (s == "Commands") {
			# 14 0D notch position (manual notch frequency)
			c = cnt[s]
			printf "Commands\\%d\\Type=Notch Filter\n", c
			printf "Commands\\%d\\String=\\\\x14\\\\x0d\n", c
			printf "Commands\\%d\\Min=0\nCommands\\%d\\Max=255\nCommands\\%d\\Bytes=0\n", c, c, c
			printf "Commands\\%d\\Command29=false\nCommands\\%d\\GetCommand=true\n", c, c
			printf "Commands\\%d\\SetCommand=true\nCommands\\%d\\Admin=false\n", c, c
		}
		if (s == "Spans") {
			ns = split("1500 1.5|3000 3|6000 6|12000 12|24000 24|48000 48", sp, "|")
			for (i = 1; i <= ns; i++) {
				split(sp[i], f, " ")
				printf "Spans\\%d\\Num=%d\nSpans\\%d\\Name=±%s kHz\nSpans\\%d\\Freq=%s\n", \
					i, i - 1, i, f[2], i, f[1]
			}
			cnt[s] = ns
		}
		printf "%s\\size=%d\n", s, s in cnt ? cnt[s] : 0
		next
	}
	p = index($0, "=")
	if (p > 1 && substr($0, 1, p - 1) in hdr) {
		printf "%s=%s\n", substr($0, 1, p - 1), hdr[substr($0, 1, p - 1)]
		next
	}
	print
}
EOF
) "${G_SRC}" "${G_SRC}"
