"""Build a fs.overlay folder that boots straight into a map (patched sequence graphs). Game-derived output: local only.

usage: work/venv/bin/python tools/dump/make_chisel_overlay.py <out_dir> --route newgame|load [--banc KokiriForest_Child] [--start NAME] [--keep-intro]
                              [--skip-title] [--skip-fades] [--slot N] [--romfs DIR]
  newgame: title menu is skipped, "New game" runs at once, ChangeBanc goes to --banc at StartPos --start
           (banc default start pos when --start is omitted); the opening events are cut unless --keep-intro.
  load:    title menu is skipped, "Continue" runs at once (save slot as the game picks it); --start puts the player
           at a named StartPos of the saved banc; --banc forces another banc (cuts BancParam from the save).
  both:    --skip-title: the title background event (Title_Background: the title movie Dm_Title_0005.webm) is neither
           reserved, waited for nor requested (BeforeTitle.110 #2). Measured SLOWER to playable (+1-2 s: the first
           scene frame then waits for ~2200 more actor files), so off by default. --skip-fades: the loading fade opens
           at once and the scene fades in at once; the load starts ~1 s earlier but ends no earlier (no gain), off by
           default. Both: work/assets/native/boot-speed.md.
Reads the sequence graphs from the config's oot dump (ounce config; --romfs: another romfs folder), and writes the resource
size table with the entries of the graphs that grew raised (tools/dump/rstb.py). Then run the game with
fs.overlay=<out_dir>.
"""
import sys, os, argparse
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def default_romfs():
    """The romfs of OoT's dump in the user's config (ounce config)."""
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    import ounce_config
    return os.path.join(ounce_config.dump('oot'), 'romfs')
sys.path.insert(0, HERE)
import ainb_patch
import rstb

SEQ = None   # (the romfs's Sequence folder: set in main)
SIZES = {}   # (romfs path -> (old size, new size) of each file emitted: their size table entries)
BP = 'Work/Banc/GameBancParam/{}.game__banc__GameBancParam.gyml'


def emit(out, name, ops):
    src = f'{SEQ}/{name}'
    data = ainb_patch.patch(open(src, 'rb').read(), ops)
    os.makedirs(f'{out}/Sequence', exist_ok=True)
    open(f'{out}/Sequence/{name}', 'wb').write(data)
    SIZES[f'Sequence/{name}'] = (os.path.getsize(src), len(data))
    print(f'{name}: {len(ops)} edits, {os.path.getsize(src)} -> {len(data)} bytes')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--route', choices=['newgame', 'load'], default='newgame')
    ap.add_argument('--banc')
    ap.add_argument('--start', default='')
    ap.add_argument('--keep-intro', action='store_true')
    ap.add_argument('--skip-title', action='store_true', help='leave out the title background event (the title movie)')
    ap.add_argument('--skip-fades', action='store_true', help='no loading fade-out before, no fade-in after the first load')
    ap.add_argument('--slot', type=int, help='load route: force SeqExecuteLoadSaveData Index (else CaptionData.SelectedSavePathIndex)')
    ap.add_argument('--romfs', help='the romfs folder (default: the config\'s oot dump + /romfs)')
    a = ap.parse_args()
    global SEQ
    romfs = a.romfs or default_romfs()
    SEQ = f'{romfs}/Sequence'
    if a.banc and not os.path.exists(f'{romfs}/Banc/GameBancParam/{a.banc}.game__banc__GameBancParam.bgyml'):
        sys.exit(f'no GameBancParam {a.banc}')

    if a.skip_title:
        # BeforeTitle.110 #2 children: 5 #34 SeqOneShotGenericReserveBootEvent('Title_Background'), 7 #35
        # SeqExecuteWaitResidentEventLoaded('Title_Background'), 9 #30 SeqOneShotRequestEvent('Title_Background'),
        # 10 #29 SeqExecuteWaitEventStart (waits for the movie to start: why HOS_NO_MOVIES=1 never leaves the boot).
        # Pointed at one-shot no-ops of the graph (#4 SeqOneShotEnd, #19 SeqOneShotFinishStartupLogo), alternated as
        # the intro cut does. The global resident events (#27, #28) still load. CloseTitle's force-skip of the event
        # then finds none.
        emit(a.out, 'BeforeTitle.110.module.ainb', ['plug:2:Child:5=19', 'plug:2:Child:7=4', 'plug:2:Child:9=4',
                                                     'plug:2:Child:10=19'])
    if a.skip_fades:
        # CloseLoadingFade #3 SeqOneShotOpenScenery FadeTime 1.667 -> 0: the scene shows at once after each load
        emit(a.out, 'CloseLoadingFade.module.ainb', ['in:3:Float:FadeTime=0'])

    if a.route == 'newgame':
        # NormalTitle #8 (save exists ? 3-button menu : 2-button menu) -> #38 (SeqOneShotEnd, Transition -> #13 new game)
        ops = ['plug:8:Child:0=38', 'plug:8:Child:1=38']
        if a.skip_fades:
            # #27 PrepareScreensOfLoading: the loading fade opened directly (IsDirect = !IsPlayLoadingFade), the
            # scenery closed in 1.0 s, not 1.7
            ops += ['in:27:Bool:IsPlayLoadingFade=false', 'in:27:Bool:IsLongFade=false']
        emit(a.out, 'NormalTitle.module.ainb', ops)
        if a.banc:
            emit(a.out, 'FormalSequence.root.ainb', [f'in:0:String:FirstBancParam={BP.format(a.banc)}'])
        ops = []
        if a.start:
            ops += ['in:2:Bool:IsOverrideTraverseBasePos=true', f'in:2:String:OverrideTraverseStartPosName={a.start}']
        if not a.keep_intro:
            # Opening #3 (ChangeBanc's DoAfterDestroyBanc): children 5 #17 event fade-out, 6 #4 request
            # 'Opening_LoadingKokiriForest', 7 #7 wait event start, 9 #27 reserve boot event 'Opening_Introduction'.
            # Point them at one-shot no-ops already in the graph (#11 SeqOneShotEnd, #12 FinishStartupLogo).
            ops += ['plug:3:Child:5=11', 'plug:3:Child:6=12', 'plug:3:Child:7=11', 'plug:3:Child:9=12']
        if ops:
            emit(a.out, 'Opening.module.ainb', ops)
    else:
        # NormalTitle #38's transition (new game) -> #42 (pause + TitleToMainWithLoadSave); #8 both ways -> #38
        emit(a.out, 'NormalTitle.module.ainb', ['plug:8:Child:0=38', 'plug:8:Child:1=38', 'plug:38:Transition:0=42'])
        ops = []
        if a.start:
            ops += [f'in:5:String:OverrideTraverseStartPosName={a.start}']
        if a.banc:
            ops += [f'unlink:5:String:BancParam={BP.format(a.banc)}']
            if not a.start:
                ops += ['in:5:Bool:IsOverrideTraverseBasePos=false']
        if ops:
            emit(a.out, 'TitleToMainWithLoadSave.module.ainb', ops)
        if a.slot is not None:
            emit(a.out, 'OnLoadGameSaveData.module.ainb', [f'unlink:2:Int:Index={a.slot}'])
    # (a graph's resource heap is its size table entry: a patched graph that grew past it fails its node allocations)
    rstb.raise_sizes(romfs, a.out, SIZES)


if __name__ == '__main__':
    main()
