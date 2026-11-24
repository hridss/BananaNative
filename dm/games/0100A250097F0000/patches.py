# DRAGON BALL FighterZ (0100A250097F0000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a
# module's NSO build ID; CUTS, OBS and FRAME_STEP as described there ("Registered patches")

_DBFZ_RES = 41   # OBS_DBFZ_RES in dm/host/dm.h

OBS = {
    '39adb54619956c8e93d3a37777f32ece': [   # v1703936 (1.33), main
        # Unreal Engine 4.19's r.SetRes sink (main+0x1e639ac): it parses the console variable's "<w>x<h>" (main+0x1e62930 into [sp+0xc] / [sp+8]) and, when that differs from
        # GSystemResolution (main+0x5bac540), resizes the game window and every render target to it. The game sets it to 1920x1080 docked and 1280x720 handheld when the operation
        # mode is applied, after the -ResX / -ResY start size (main+0x8d0b60). The gate after the parse (ldr w9,[sp,#4]; x29 = sp + 0x30) lets the host make the width the
        # picture's shape times the height (dm/host/android/dbfz_aspect.c): the whole game then renders at that shape (1920x1440 docked at 4:3), its cameras and its UI laid out for it
        (0x1e63a34, _DBFZ_RES, 'b94007e9', "r.SetRes parsed (ldr w9,[sp,#4]); x29 the frame (width at x29-0x24, height at x29-0x28)"),
        # FMinimalViewInfo::CalculateProjectionMatrixGivenView (main+0x18c87c4): bConstrainAspectRatio (bit 0 of [x0+0x2c]) sends the view to FViewport::CalculateViewExtents
        # (main+0x1e61968), which letterboxes a camera's 16:9 inside a 4:3 window (black bars, the HUD inside them). The gate on that load (ldrb w8,[x20,#0x2c]) lets the host
        # clear the bit and pick AspectRatio_MaintainXFOV ([x1]) at a picture shape: the camera keeps its horizontal reach and sees more above and below
        (0x18c87e0, _DBFZ_RES | 1 << 8, '3940b288', "camera projection: bConstrainAspectRatio about to be read (ldrb w8,[x20,#0x2c]); x20 the view info, x1 the aspect axis constraint"),
    ],
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md)
BINDING = [
    {'what': "r.SetRes sink: the window's width follows the picture's shape", 'now': 'build', 'goal': 'engine', 'purpose': 'rendering',
     'why': "Unreal Engine 4's FSystemResolution sink; a function match could find it in other Unreal Engine 4 games"},
    {'what': "camera projection: no letterbox at the picture's shape", 'now': 'build', 'goal': 'engine', 'purpose': 'rendering',
     'why': "Unreal Engine 4's FMinimalViewInfo::CalculateProjectionMatrixGivenView; a function match could find it in other Unreal Engine 4 games"},
]
