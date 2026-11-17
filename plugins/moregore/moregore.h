// shared between more gore's files.
#pragma once
#include "../../include/macvc.h"
#include <stdint.h>

extern int g_off_part, g_off_health, g_off_invehicle, g_ped_size;
extern uint8_t **PedPool;                 // CPools::ms_pPedPool: entries +0, flags +8, size +0x10
extern void *(*FindPlayerPed)(void);
int is_ped(uint8_t *e);

// blood.c
void blood_init(void);                    // after the offsets above are known
void blood_menu(int page);
void blood_melee_hit(uint8_t *ped, int weapon); // from the InflictDamage hook
int blood_level(void);                    // 0 vanilla, 1 more, 2 extreme

// limbs.c
void limbs_init(int off_clump);
void limbs_menu(int page);
void limbs_spawn(uint8_t *ped, int node, float dx, float dy); // a part just came off; (dx, dy) = which way

