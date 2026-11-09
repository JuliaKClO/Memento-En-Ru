# Provenance and redistribution review

| Component | Evidence / license | Release handling |
|---|---|---|
| Memento 2.0.2, Ripose | LICENSE GPL v2; source headers explicitly version 2 | Preserve notices; do not silently relicense to GPLv3 |
| QCoro v0.13.0 | pinned commit 98f23d8d8d67e9e24217d268927faec97c713fb0; MIT LICENSE in upstream source | Fetch dependency separately; preserve MIT notice with binaries |
| irregular verb data | src/dict/data/IRREGULAR_VERBS_LICENSE.txt, MIT attribution | Retained with original notice |
| Upstream SVG/icons | verified upstream archive provenance in original audit | Original notices retained; no assertion of new authorship |
| flag-us, flag-uk SVG | New geometric drawings produced during this preparation; no old flag SVG copied | Distributed under project GPLv2; simplified flag artwork |
| ai-assistant SVG | Restored from the preserved updates source; provenance of this fork addition is not independently established | Preserve the existing project notices; owner provenance review remains open |
| Lingvo LSA/parser and added fork code | Existing notices retained; authorship/provenance of prior additions not independently established | Owner review required; do not invent attribution |
| SoundEn.lsa and extracted audio | No redistribution grant established locally | Excluded; user supplies archive separately |
| Lingvo 14643-row audio mapping | Restored from the earlier user-provided build and embedded as a resource; provenance and redistribution rights are not established | Included at project owner's direction; review rights before public redistribution |
| IPADIC | No local license established for archive's 9 files | Excluded; MeCab off |
| User dictionaries, Anki decks/config/addons | No grants established | Excluded; install separately |
| Qt6 MSYS2 | GPL/LGPL and component-specific licenses in package license directories | Binary compatibility/source obligations require review; no public binary approved |
| mpv 0.41.0-8 | MSYS2 metadata GPL-2.0-or-later | Actual linked FFmpeg license matters |
| FFmpeg 9.0.2-1 | Installed package metadata GPL-3.0-or-later | Known GPLv2-only/GPLv3 conflict accepted by the owner as a project risk; compatibility is not confirmed, and technical preparation continues |
| libass 0.17.5 | MSYS2 metadata ISC | Preserve license in permitted bundle |
| libarchive 3.8.10 | MSYS2 metadata BSD-2-Clause | Preserve license |
| libzip 1.12 | MSYS2 metadata BSD-3-Clause | Preserve license |
| libdvdcss and remaining transitive DLLs | Must map exact runtime dependency-edges.csv to package license/source records | No approval based on presence in a personal bundle |

Package metadata is evidence about installed binaries, not proof of every component's legal status. Full corresponding sources and build configurations must be made available as required before distribution; links alone are not a substitute for satisfying applicable source obligations.

Authoritative references: https://www.gnu.org/licenses/gpl-faq.html#v2v3Compatibility and https://ffmpeg.org/doxygen/trunk/md_LICENSE.html . GPLv2-only/GPLv3 compatibility cannot be solved by changing this project's LICENSE unilaterally. No questionable data was added merely to match the personal release.
