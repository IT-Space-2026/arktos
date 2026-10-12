# Devin autonomous task list — Arora LANG LANE (branch lang-work, worktree ../arora-lang)

Translation queue driven by `.devin/taskloop-lang.sh`. Scope: complete the
unfinished <translation> entries of the bundled UI catalogs in src/locale/
(one locale per task, one task per run). Each run works the FIRST row whose
Status is pending or in_progress, marks it done, commits to THIS branch
(lang-work), and stops.

Status values: `pending` | `in_progress` | `done` | `blocked`

Rules for the agent working this file:
- Mark the row `in_progress` before starting work, `done` only after
  lrelease-validating the catalog per the row's verification hint.
- `blocked:<reason>` in Notes when genuinely stuck — never fake done.
- One task per run: after committing, STOP.
- Never revert unrelated working-tree changes.
- Append a one-line summary to `.devin/WORKLOG.md` when a task completes.
- Format: the table holds a one-line summary per row; the FULL spec
  lives in the `### <id>` section under `## Task details`. Keep table
  cells short — long text belongs in the details section.
- Edit ONLY the .ts file the row names + .devin bookkeeping files.

Context: repo is Arora, a Qt WebKit-era browser (upstream dead ~2010),
ported to Qt 6.12.0 + QtWebEngine. Catalogs were refreshed by lupdate —
every untranslated source string sits as type="unfinished". Qt lives at
~/Qt/6.12.0/gcc_64 (lrelease at ~/Qt/6.12.0/gcc_64/bin/lrelease). See
`.devin/RUNNER_PROMPT_LANG.md` for the translation quality bar.

| ID | Task | Status | Attempts | Notes |
|----|------|--------|----------|-------|
| TRANS01 | Complete hu_HU UI translation — details below | done | 1 | All 1081 unfinished entries translated to Hungarian (incl. 1 numerusform); lrelease PASS — 1423 finished, 0 unfinished. |
| TRANS02 | Complete ast UI translation — details below | done | 1 | All 118 unfinished entries translated to Asturian (incl. 4 numerusform); lrelease PASS — 448 finished, 0 unfinished. |
| TRANS03 | Complete ca UI translation — details below | done | 1 | All 1092 unfinished entries translated to Catalan (incl. 3 numerusform); lrelease PASS — 1423 finished, 0 unfinished. |
| TRANS04 | Complete cs_CZ UI translation — details below | done | 1 | All 1033 unfinished entries translated to Czech (incl. 1 numerusform); lrelease PASS — 1423 finished, 0 unfinished. |
| TRANS05 | Complete da_DK UI translation — details below | done | 1 | All 1212 unfinished entries translated to Danish (incl. 4 numerusform); lrelease PASS — 1423 finished, 0 unfinished. |
| TRANS06 | Complete de_DE UI translation — details below | done | 1 | All 1033 unfinished entries translated to German (incl. 1 numerusform); lrelease PASS — 1423 finished, 0 unfinished. |
| TRANS07 | Complete el_GR UI translation — details below | done | 1 | All 1120 unfinished entries translated to Greek (incl. 1 numerusform); lrelease PASS — 1423 finished, 0 unfinished. |
| TRANS08 | Complete es UI translation — details below | done | 1 | All 1036 unfinished entries translated to Spanish (incl. 4 numerusform); lrelease PASS — 1423 finished, 0 unfinished. |
| TRANS09 | Complete et_EE UI translation — details below | done | 1 | All 1210 unfinished entries translated to Estonian (incl. 3 numerusform); lrelease PASS — 1423 finished, 0 unfinished. |
| TRANS10 | Complete fi_FI UI translation — details below | done | 1 | All 1120 unfinished entries translated to Finnish (incl. 1 numerusform); lrelease PASS — 1423 finished, 0 unfinished. |
| TRANS11 | Complete fr_FR UI translation — details below | done | 1 | All 1033 unfinished entries translated to French (incl. 1 numerusform); lrelease PASS — 1423 finished, 0 unfinished. |
| TRANS12 | Complete gl UI translation — details below | done | 1 | All 1036 unfinished entries translated to Galician (incl. 4 numerusform); lrelease PASS — 1423 finished, 0 unfinished. |
| TRANS13 | Complete it_IT UI translation — details below | done | 1 | All 1066 unfinished entries translated to Italian (incl. 1 numerusform); lrelease PASS — 1423 finished, 0 unfinished. |
| TRANS14 | Complete ja_JP UI translation — details below | done | 1 | All 1033 unfinished entries translated to Japanese (incl. 1 numerusform); lrelease PASS — 1423 finished, 0 unfinished. |
| TRANS15 | Complete nb_NO UI translation — details below | done | 1 | All 1035 unfinished entries translated to Norwegian Bokmål (incl. 1 numerusform); lrelease PASS — 1423 finished, 0 unfinished. |
| TRANS16 | Complete nl UI translation — details below | done | 1 | All 1035 unfinished entries translated to Dutch (incl. 1 numerusform); lrelease PASS — 1423 finished, 0 unfinished. |
| TRANS17 | Complete pl_PL UI translation — details below | pending | 0 |  |
| TRANS18 | Complete pt_PT UI translation — details below | pending | 0 |  |
| TRANS19 | Complete sr_RS UI translation — details below | pending | 0 |  |
| TRANS20 | Complete sr_RS@latin UI translation — details below | pending | 0 |  |
| TRANS21 | Complete tr_TR UI translation — details below | pending | 0 |  |


## Task details

Full task specifications, keyed by row id.

### TRANS01

Complete hu_HU UI translation — translate every type="unfinished" entry in src/locale/hu_HU.ts to hu_HU per the lane prompt (preserve %N placeholders, &amp; accelerators, entities, embedded HTML; match the register of existing translated entries; remove unfinished marking when done; ambiguous leftovers documented). Verify: lrelease hu_HU.ts -qm /tmp/x.qm exits 0; unfinished count 0 or leftovers noted.

### TRANS02

Complete ast UI translation — translate every type="unfinished" entry in src/locale/ast.ts to ast per the lane prompt (preserve %N placeholders, &amp; accelerators, entities, embedded HTML; match the register of existing translated entries; remove unfinished marking when done; ambiguous leftovers documented). Verify: lrelease ast.ts -qm /tmp/x.qm exits 0; unfinished count 0 or leftovers noted.

### TRANS03

Complete ca UI translation — translate every type="unfinished" entry in src/locale/ca.ts to ca per the lane prompt (preserve %N placeholders, &amp; accelerators, entities, embedded HTML; match the register of existing translated entries; remove unfinished marking when done; ambiguous leftovers documented). Verify: lrelease ca.ts -qm /tmp/x.qm exits 0; unfinished count 0 or leftovers noted.

### TRANS04

Complete cs_CZ UI translation — translate every type="unfinished" entry in src/locale/cs_CZ.ts to cs_CZ per the lane prompt (preserve %N placeholders, &amp; accelerators, entities, embedded HTML; match the register of existing translated entries; remove unfinished marking when done; ambiguous leftovers documented). Verify: lrelease cs_CZ.ts -qm /tmp/x.qm exits 0; unfinished count 0 or leftovers noted.

### TRANS05

Complete da_DK UI translation — translate every type="unfinished" entry in src/locale/da_DK.ts to da_DK per the lane prompt (preserve %N placeholders, &amp; accelerators, entities, embedded HTML; match the register of existing translated entries; remove unfinished marking when done; ambiguous leftovers documented). Verify: lrelease da_DK.ts -qm /tmp/x.qm exits 0; unfinished count 0 or leftovers noted.

### TRANS06

Complete de_DE UI translation — translate every type="unfinished" entry in src/locale/de_DE.ts to de_DE per the lane prompt (preserve %N placeholders, &amp; accelerators, entities, embedded HTML; match the register of existing translated entries; remove unfinished marking when done; ambiguous leftovers documented). Verify: lrelease de_DE.ts -qm /tmp/x.qm exits 0; unfinished count 0 or leftovers noted.

### TRANS07

Complete el_GR UI translation — translate every type="unfinished" entry in src/locale/el_GR.ts to el_GR per the lane prompt (preserve %N placeholders, &amp; accelerators, entities, embedded HTML; match the register of existing translated entries; remove unfinished marking when done; ambiguous leftovers documented). Verify: lrelease el_GR.ts -qm /tmp/x.qm exits 0; unfinished count 0 or leftovers noted.

### TRANS08

Complete es UI translation — translate every type="unfinished" entry in src/locale/es.ts to es per the lane prompt (preserve %N placeholders, &amp; accelerators, entities, embedded HTML; match the register of existing translated entries; remove unfinished marking when done; ambiguous leftovers documented). Verify: lrelease es.ts -qm /tmp/x.qm exits 0; unfinished count 0 or leftovers noted.

### TRANS09

Complete et_EE UI translation — translate every type="unfinished" entry in src/locale/et_EE.ts to et_EE per the lane prompt (preserve %N placeholders, &amp; accelerators, entities, embedded HTML; match the register of existing translated entries; remove unfinished marking when done; ambiguous leftovers documented). Verify: lrelease et_EE.ts -qm /tmp/x.qm exits 0; unfinished count 0 or leftovers noted.

### TRANS10

Complete fi_FI UI translation — translate every type="unfinished" entry in src/locale/fi_FI.ts to fi_FI per the lane prompt (preserve %N placeholders, &amp; accelerators, entities, embedded HTML; match the register of existing translated entries; remove unfinished marking when done; ambiguous leftovers documented). Verify: lrelease fi_FI.ts -qm /tmp/x.qm exits 0; unfinished count 0 or leftovers noted.

### TRANS11

Complete fr_FR UI translation — translate every type="unfinished" entry in src/locale/fr_FR.ts to fr_FR per the lane prompt (preserve %N placeholders, &amp; accelerators, entities, embedded HTML; match the register of existing translated entries; remove unfinished marking when done; ambiguous leftovers documented). Verify: lrelease fr_FR.ts -qm /tmp/x.qm exits 0; unfinished count 0 or leftovers noted.

### TRANS12

Complete gl UI translation — translate every type="unfinished" entry in src/locale/gl.ts to gl per the lane prompt (preserve %N placeholders, &amp; accelerators, entities, embedded HTML; match the register of existing translated entries; remove unfinished marking when done; ambiguous leftovers documented). Verify: lrelease gl.ts -qm /tmp/x.qm exits 0; unfinished count 0 or leftovers noted.

### TRANS13

Complete it_IT UI translation — translate every type="unfinished" entry in src/locale/it_IT.ts to it_IT per the lane prompt (preserve %N placeholders, &amp; accelerators, entities, embedded HTML; match the register of existing translated entries; remove unfinished marking when done; ambiguous leftovers documented). Verify: lrelease it_IT.ts -qm /tmp/x.qm exits 0; unfinished count 0 or leftovers noted.

### TRANS14

Complete ja_JP UI translation — translate every type="unfinished" entry in src/locale/ja_JP.ts to ja_JP per the lane prompt (preserve %N placeholders, &amp; accelerators, entities, embedded HTML; match the register of existing translated entries; remove unfinished marking when done; ambiguous leftovers documented). Verify: lrelease ja_JP.ts -qm /tmp/x.qm exits 0; unfinished count 0 or leftovers noted.

### TRANS15

Complete nb_NO UI translation — translate every type="unfinished" entry in src/locale/nb_NO.ts to nb_NO per the lane prompt (preserve %N placeholders, &amp; accelerators, entities, embedded HTML; match the register of existing translated entries; remove unfinished marking when done; ambiguous leftovers documented). Verify: lrelease nb_NO.ts -qm /tmp/x.qm exits 0; unfinished count 0 or leftovers noted.

### TRANS16

Complete nl UI translation — translate every type="unfinished" entry in src/locale/nl.ts to nl per the lane prompt (preserve %N placeholders, &amp; accelerators, entities, embedded HTML; match the register of existing translated entries; remove unfinished marking when done; ambiguous leftovers documented). Verify: lrelease nl.ts -qm /tmp/x.qm exits 0; unfinished count 0 or leftovers noted.

### TRANS17

Complete pl_PL UI translation — translate every type="unfinished" entry in src/locale/pl_PL.ts to pl_PL per the lane prompt (preserve %N placeholders, &amp; accelerators, entities, embedded HTML; match the register of existing translated entries; remove unfinished marking when done; ambiguous leftovers documented). Verify: lrelease pl_PL.ts -qm /tmp/x.qm exits 0; unfinished count 0 or leftovers noted.

### TRANS18

Complete pt_PT UI translation — translate every type="unfinished" entry in src/locale/pt_PT.ts to pt_PT per the lane prompt (preserve %N placeholders, &amp; accelerators, entities, embedded HTML; match the register of existing translated entries; remove unfinished marking when done; ambiguous leftovers documented). Verify: lrelease pt_PT.ts -qm /tmp/x.qm exits 0; unfinished count 0 or leftovers noted.

### TRANS19

Complete sr_RS UI translation — translate every type="unfinished" entry in src/locale/sr_RS.ts to sr_RS per the lane prompt (preserve %N placeholders, &amp; accelerators, entities, embedded HTML; match the register of existing translated entries; remove unfinished marking when done; ambiguous leftovers documented). Verify: lrelease sr_RS.ts -qm /tmp/x.qm exits 0; unfinished count 0 or leftovers noted.

### TRANS20

Complete sr_RS@latin UI translation — translate every type="unfinished" entry in src/locale/sr_RS@latin.ts to sr_RS@latin per the lane prompt (preserve %N placeholders, &amp; accelerators, entities, embedded HTML; match the register of existing translated entries; remove unfinished marking when done; ambiguous leftovers documented). Verify: lrelease sr_RS@latin.ts -qm /tmp/x.qm exits 0; unfinished count 0 or leftovers noted.

### TRANS21

Complete tr_TR UI translation — translate every type="unfinished" entry in src/locale/tr_TR.ts to tr_TR per the lane prompt (preserve %N placeholders, &amp; accelerators, entities, embedded HTML; match the register of existing translated entries; remove unfinished marking when done; ambiguous leftovers documented). Verify: lrelease tr_TR.ts -qm /tmp/x.qm exits 0; unfinished count 0 or leftovers noted.
