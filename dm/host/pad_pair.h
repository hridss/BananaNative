// Two input devices as the two halves of one controller (input_android.c; pure, so tests/t_hid.c checks it without controllers). Android reports a Joy-Con L and a
// Joy-Con R as two gamepads; the launcher pairs them when they connect (PadOrder.java, the "Two Joy-Cons, one controller" setting) and says so with
// nxhost_input_pair. Everything else follows from the pair: both halves feed one player, the left half's stick is the left stick and the right half's the right
// one, and a half that goes leaves the player to the other half, which then plays alone.
#pragma once
#include "dm_input.h"

enum { PAD_PAIRS = DM_PLAYERS / 2 };   // one pair per two players at most
typedef struct { int left[PAD_PAIRS], right[PAD_PAIRS]; } pad_pairs;   // device ids, -1 when the pair is free

static void pad_pairs_clear(pad_pairs *p) { for (int i = 0; i < PAD_PAIRS; i++) p->left[i] = p->right[i] = -1; }
static int pad_pair_at(const pad_pairs *p, int device) {
    for (int i = 0; device >= 0 && i < PAD_PAIRS; i++) if (p->left[i] == device || p->right[i] == device) return i;
    return -1;
}
static int pad_pair_mate(const pad_pairs *p, int device) {   // the other half's device id, -1 when the device is not paired
    int i = pad_pair_at(p, device); if (i < 0) return -1;
    return p->left[i] == device ? p->right[i] : p->left[i];
}
static int pad_pair_side(const pad_pairs *p, int device) {   // -1 the left half, 1 the right, 0 not paired
    int i = pad_pair_at(p, device); if (i < 0) return 0;
    return p->left[i] == device ? -1 : 1;
}
static int pad_pair_remove(pad_pairs *p, int device) {   // the mate the device had, -1 when it was not paired
    int i = pad_pair_at(p, device); if (i < 0) return -1;
    int mate = p->left[i] == device ? p->right[i] : p->left[i];
    p->left[i] = p->right[i] = -1;
    return mate;
}
static int pad_pair_add(pad_pairs *p, int left, int right) {   // 1 when the two are paired now; a device is in one pair at most
    if (left < 0 || right < 0 || left == right) return 0;
    pad_pair_remove(p, left); pad_pair_remove(p, right);
    for (int i = 0; i < PAD_PAIRS; i++) if (p->left[i] < 0) { p->left[i] = left; p->right[i] = right; return 1; }
    return 0;
}