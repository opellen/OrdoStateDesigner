# Vendored brand mark

The approved Ordo Structured O brand mark, campaign `logo`
(`.openillust/campaigns/logo/plans/2026-09-25-state-designer-lockup.md`,
approval in `.openillust/campaigns/logo/approvals.md`).
Source of truth for REGENERATING art: `.openillust/campaigns/logo/`
(campaign.yaml contract, plans/, QC-gated).
Source of truth for BUILDING: this directory, embedded via `../brand.qrc`.

`logo-mark.svg`: viewBox 1024, four filled paths, TWO brand colours
(`#2D63D7` Ordo Blue on one segment, `#1F2933` Ordo Charcoal on the other
three) -- unlike `src/resources/icons/`, this asset is NOT runtime-tinted;
`view/shell/icons.cpp`'s `appIcon()` renders it in its own colours as-is.

`app/state-designer.ico` (the Windows exe's file icon) is DERIVED from
this SVG by `python tools/app-icon/make_ico.py`. A change here means:
re-copy the approved mark from the campaign, then re-run that script to
rebuild the `.ico`.

Do not hand-edit; regenerate through `/opil:*` and re-copy.
