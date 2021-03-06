/***********************************************************************
 Freeciv - Copyright (C) 1996 - A Kjeldberg, L Gregersen, P Unold
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
***********************************************************************/

#ifdef HAVE_CONFIG_H
#include <fc_config.h>
#endif

#include <stdio.h>

/* utility */
#include "bitvector.h"
#include "fcintl.h"
#include "log.h"
#include "rand.h"

/* common */
#include "base.h"
#include "events.h"
#include "game.h"
#include "government.h"
#include "map.h"
#include "movement.h"
#include "player.h"
#include "research.h"
#include "unitlist.h"

/* server */
#include "actiontools.h"
#include "aiiface.h"
#include "citytools.h"
#include "cityturn.h"
#include "diplhand.h"
#include "diplomats.h"
#include "notify.h"
#include "plrhand.h"
#include "techtools.h"
#include "unithand.h"
#include "unittools.h"

/* server/scripting */
#include "script_server.h"

/****************************************************************************/

static void diplomat_charge_movement (struct unit *pdiplomat,
                                      struct tile *ptile);
static bool diplomat_success_vs_defender(struct unit *patt, struct unit *pdef,
                                         struct tile *pdefender_tile);
static bool diplomat_infiltrate_tile(struct player *pplayer,
                                     struct player *cplayer,
                                     struct unit *pdiplomat,
                                     struct unit *pvictim,
                                     struct tile *ptile);
static void diplomat_escape(struct player *pplayer, struct unit *pdiplomat,
                            const struct city *pcity);
static void diplomat_escape_full(struct player *pplayer,
                                 struct unit *pdiplomat,
                                 bool city_related,
                                 struct tile *ptile,
                                 const char *vlink);

/******************************************************************************
  Poison a city's water supply.

  - Check for infiltration success.  Our poisoner may not survive this.
  - If successful, reduces population by one point.

  - The poisoner may be captured and executed, or escape to its home town.

  'pplayer' is the player who tries to poison 'pcity' with its unit
  'pdiplomat'.
****************************************************************************/
void spy_poison(struct player *pplayer, struct unit *pdiplomat,
		struct city *pcity)
{
  struct player *cplayer;
  struct tile *ctile;
  const char *clink;

  /* Fetch target city's player.  Sanity checks. */
  fc_assert_ret(pcity);
  cplayer = city_owner(pcity);
  fc_assert_ret(cplayer);

  /* Sanity check: The actor still exists. */
  fc_assert_ret(pplayer);
  fc_assert_ret(pdiplomat);

  ctile = city_tile(pcity);
  clink = city_link(pcity);

  log_debug("poison: unit: %d", pdiplomat->id);

  /* Check if the Diplomat/Spy succeeds against defending Diplomats/Spies. */
  if (!diplomat_infiltrate_tile(pplayer, cplayer,
                                pdiplomat, NULL, ctile)) {
    return;
  }

  log_debug("poison: infiltrated");
  log_debug("poison: succeeded");

  /* Poison people! */
  if (city_reduce_size(pcity, 1, pplayer, "poison")) {
    /* Notify everybody involved. */
    notify_player(pplayer, ctile, E_MY_DIPLOMAT_POISON, ftc_server,
                  _("Your %s poisoned the water supply of %s."),
                  unit_link(pdiplomat), clink);
    notify_player(cplayer, ctile,
                  E_ENEMY_DIPLOMAT_POISON, ftc_server,
                  _("%s is suspected of poisoning the water supply of %s."),
                  player_name(pplayer), clink);

    /* Update clients. */
    city_refresh (pcity);
    send_city_info(NULL, pcity);
  } else {
    /* Notify everybody involved. */
    notify_player(pplayer, ctile, E_MY_DIPLOMAT_POISON, ftc_server,
                  _("Your %s destroyed %s by poisoning its water supply."),
                  unit_link(pdiplomat), clink);
    notify_player(cplayer, ctile,
                  E_ENEMY_DIPLOMAT_POISON, ftc_server,
                  _("%s is suspected of destroying %s by poisoning its"
                    " water supply."),
                  player_name(pplayer), clink);
  }

  /* this may cause a diplomatic incident */
  action_consequence_success(ACTION_SPY_POISON, pplayer, cplayer,
                             ctile, clink);

  /* Now lets see if the spy survives. */
  diplomat_escape_full(pplayer, pdiplomat, TRUE, ctile, clink);
}

/******************************************************************************
  Investigate a city.

  - It costs some minimal movement to investigate a city.

  - Diplomats die after investigation.
  - Spies always survive.  There is no risk.
****************************************************************************/
void diplomat_investigate(struct player *pplayer, struct unit *pdiplomat,
			  struct city *pcity)
{
  struct player *cplayer;
  struct packet_unit_short_info unit_packet;
  struct packet_city_info city_packet;

  /* Fetch target city's player.  Sanity checks. */
  fc_assert_ret(pcity);
  cplayer = city_owner(pcity);
  fc_assert_ret(cplayer);

  /* Sanity check: The actor still exists. */
  fc_assert_ret(pplayer);
  fc_assert_ret(pdiplomat);

  /* Sanity check: The target is foreign. */
  if (cplayer == pplayer) {
    /* Nothing to do to a domestic target. */
    return;
  }

  log_debug("investigate: unit: %d", pdiplomat->id);

  /* Do It... */
  update_dumb_city(pplayer, pcity);
  /* Special case for a diplomat/spy investigating a city:
     The investigator needs to know the supported and present
     units of a city, whether or not they are fogged. So, we
     send a list of them all before sending the city info.
     As this is a special case we bypass send_unit_info. */
  unit_list_iterate(pcity->units_supported, punit) {
    package_short_unit(punit, &unit_packet,
                       UNIT_INFO_CITY_SUPPORTED, pcity->id);
    /* We need to force to send the packet to ensure the client will receive
     * something (e.g. investigating twice). */
    lsend_packet_unit_short_info(pplayer->connections, &unit_packet, TRUE);
  } unit_list_iterate_end;
  unit_list_iterate((pcity->tile)->units, punit) {
    package_short_unit(punit, &unit_packet,
                       UNIT_INFO_CITY_PRESENT, pcity->id);
    /* We need to force to send the packet to ensure the client will receive
     * something (e.g. investigating twice). */
    lsend_packet_unit_short_info(pplayer->connections, &unit_packet, TRUE);
  } unit_list_iterate_end;
  /* Send city info to investigator's player.
     As this is a special case we bypass send_city_info. */
  package_city(pcity, &city_packet, TRUE);
  /* We need to force to send the packet to ensure the client will receive
   * something and popup the city dialog. */
  lsend_packet_city_info(pplayer->connections, &city_packet, TRUE);

  /* Charge a nominal amount of movement for this. */
  (pdiplomat->moves_left)--;
  if (pdiplomat->moves_left < 0) {
    pdiplomat->moves_left = 0;
  }

  /* this may cause a diplomatic incident */
  action_consequence_success(ACTION_SPY_INVESTIGATE_CITY, pplayer, cplayer,
                             city_tile(pcity), city_link(pcity));

  /* Spies always survive. Diplomats never do. */
  if (!unit_has_type_flag(pdiplomat, UTYF_SPY)) {
    wipe_unit(pdiplomat, ULR_USED, NULL);
  } else {
    send_unit_info(NULL, pdiplomat);
  }
}

/******************************************************************************
  Get list of improvements from city (for purposes of sabotage).

  - Always successful; returns list.

  Only send back to the originating connection, if there is one. (?)
****************************************************************************/
void spy_send_sabotage_list(struct connection *pc, struct unit *pdiplomat,
			    struct city *pcity)
{
  struct packet_city_sabotage_list packet;

  /* Send city improvements info to player. */
  BV_CLR_ALL(packet.improvements);

  improvement_iterate(ptarget) {
    if (city_has_building(pcity, ptarget)) {
      BV_SET(packet.improvements, improvement_index(ptarget));
    }
  } improvement_iterate_end;

  packet.diplomat_id = pdiplomat->id;
  packet.city_id = pcity->id;
  send_packet_city_sabotage_list(pc, &packet);
}

/******************************************************************************
  Establish an embassy.

  - Barbarians always execute ambassadors.
  - Otherwise, the embassy is created.
  - It costs some minimal movement to establish an embassy.

  - Diplomats are consumed in creation of embassy.
  - Spies always survive.
****************************************************************************/
void diplomat_embassy(struct player *pplayer, struct unit *pdiplomat,
		      struct city *pcity)
{
  struct player *cplayer;

  /* Fetch target city's player.  Sanity checks. */
  fc_assert_ret(pcity);
  cplayer = city_owner(pcity);
  fc_assert_ret(cplayer);

  /* Sanity check: The actor still exists. */
  fc_assert_ret(pplayer);
  fc_assert_ret(pdiplomat);

  /* Sanity check: The target is foreign. */
  if (cplayer == pplayer) {
    /* Nothing to do to a domestic target. */
    return;
  }

  log_debug("embassy: unit: %d", pdiplomat->id);

  log_debug("embassy: succeeded");

  establish_embassy(pplayer, cplayer);

  /* Notify everybody involved. */
  notify_player(pplayer, city_tile(pcity),
                E_MY_DIPLOMAT_EMBASSY, ftc_server,
                _("You have established an embassy in %s."),
                city_link(pcity));
  notify_player(cplayer, city_tile(pcity),
                E_ENEMY_DIPLOMAT_EMBASSY, ftc_server,
                _("The %s have established an embassy in %s."),
                nation_plural_for_player(pplayer),
                city_link(pcity));

  /* Charge a nominal amount of movement for this. */
  (pdiplomat->moves_left)--;
  if (pdiplomat->moves_left < 0) {
    pdiplomat->moves_left = 0;
  }

  /* this may cause a diplomatic incident */
  action_consequence_success(ACTION_ESTABLISH_EMBASSY, pplayer, cplayer,
                             city_tile(pcity), city_link(pcity));

  /* Spies always survive. Diplomats never do. */
  if (!unit_has_type_flag(pdiplomat, UTYF_SPY)) {
    wipe_unit(pdiplomat, ULR_USED, NULL);
  } else {
    send_unit_info(NULL, pdiplomat);
  }
}

/******************************************************************************
  Sabotage an enemy unit.

  - If successful, reduces hit points by half of those remaining.

  - The saboteur may be captured and executed, or escape to its home town.
****************************************************************************/
void spy_sabotage_unit(struct player *pplayer, struct unit *pdiplomat,
		       struct unit *pvictim)
{
  char victim_link[MAX_LEN_LINK];
  struct player *uplayer;

  /* Fetch target unit's player.  Sanity checks. */
  fc_assert_ret(pvictim);
  uplayer = unit_owner(pvictim);
  fc_assert_ret(uplayer);

  /* Sanity check: The actor still exists. */
  fc_assert_ret(pplayer);
  fc_assert_ret(pdiplomat);

  log_debug("sabotage-unit: unit: %d", pdiplomat->id);

  /* N.B: unit_link() always returns the same pointer. */
  sz_strlcpy(victim_link, unit_link(pvictim));

  /* Diplomatic battle against any diplomatic defender except the intended
   * victim of the sabotage. */
  if (!diplomat_infiltrate_tile(pplayer, uplayer, pdiplomat, pvictim,
                                unit_tile(pvictim))) {
    return;
  }

  log_debug("sabotage-unit: succeeded");

  if (pvictim->hp < 2) {
    /* Not possible to halve the hit points. Kill it. */
    wipe_unit(pvictim, ULR_KILLED, pplayer);

    /* Notify everybody involved. */
    notify_player(pplayer, unit_tile(pvictim),
                  E_MY_DIPLOMAT_SABOTAGE, ftc_server,
                  _("Your %s's successful sabotage killed the %s %s."),
                  unit_link(pdiplomat),
                  nation_adjective_for_player(uplayer),
                  victim_link);
    notify_player(uplayer, unit_tile(pvictim),
                  E_ENEMY_DIPLOMAT_SABOTAGE, ftc_server,
                  /* TRANS: ... the Poles! */
                  _("Your %s was killed by %s sabotage!"),
                  victim_link,
                  nation_plural_for_player(pplayer));
  } else {
    /* Sabotage the unit by removing half its remaining hit points. */
    pvictim->hp /= 2;
    send_unit_info(NULL, pvictim);

    /* Notify everybody involved. */
    notify_player(pplayer, unit_tile(pvictim),
                  E_MY_DIPLOMAT_SABOTAGE, ftc_server,
                  _("Your %s succeeded in sabotaging the %s %s."),
                  unit_link(pdiplomat),
                  nation_adjective_for_player(uplayer),
                  victim_link);
    notify_player(uplayer, unit_tile(pvictim),
                  E_ENEMY_DIPLOMAT_SABOTAGE, ftc_server,
                  /* TRANS: ... the Poles! */
                  _("Your %s was sabotaged by the %s!"),
                  victim_link,
                  nation_plural_for_player(pplayer));
  }

  /* this may cause a diplomatic incident */
  action_consequence_success(ACTION_SPY_SABOTAGE_UNIT, pplayer, uplayer,
                             unit_tile(pvictim), victim_link);

  /* Now lets see if the spy survives. */
  diplomat_escape(pplayer, pdiplomat, NULL);
}

/******************************************************************************
  Bribe an enemy unit.
  
  - Can't bribe a unit if:
    - Player doesn't have enough gold.
  - Otherwise, the unit will be bribed.

  - A successful briber will try to move onto the victim's square.
****************************************************************************/
void diplomat_bribe(struct player *pplayer, struct unit *pdiplomat,
		    struct unit *pvictim)
{
  char victim_link[MAX_LEN_LINK];
  struct player *uplayer;
  struct tile *victim_tile;
  int bribe_cost;
  int diplomat_id = pdiplomat->id;
  struct city *pcity;
  bool bounce;

  /* Fetch target unit's player.  Sanity checks. */
  fc_assert_ret(pvictim);
  uplayer = unit_owner(pvictim);
  fc_assert_ret(uplayer);

  /* Sanity check: The actor still exists. */
  fc_assert_ret(pplayer);
  fc_assert_ret(pdiplomat);

  /* Sanity check: The target is foreign. */
  if (uplayer == pplayer) {
    /* Nothing to do to a domestic target. */
    return;
  }

  /* Sanity check: The victim isn't a unique unit the actor player already
   * has. */
  if (utype_player_already_has_this_unique(pplayer,
                                           unit_type_get(pvictim))) {
    log_debug("bribe-unit: already got unique unit");
    notify_player(pplayer, unit_tile(pdiplomat),
                  E_UNIT_ILLEGAL_ACTION, ftc_server,
                  /* TRANS: You already have a Leader. */
                  _("You already have a %s."),
                  unit_link(pvictim));

    return;
  }

  log_debug("bribe-unit: unit: %d", pdiplomat->id);

  /* Get bribe cost, ignoring any previously saved value. */
  bribe_cost = unit_bribe_cost(pvictim, pplayer);

  /* If player doesn't have enough gold, can't bribe. */
  if (pplayer->economic.gold < bribe_cost) {
    notify_player(pplayer, unit_tile(pdiplomat),
                  E_MY_DIPLOMAT_FAILED, ftc_server,
                  _("You don't have enough gold to bribe the %s %s."),
                  nation_adjective_for_player(uplayer),
                  unit_link(pvictim));
    log_debug("bribe-unit: not enough gold");
    return;
  }

  log_debug("bribe-unit: enough gold");

  /* Diplomatic battle against any diplomatic defender except the one that
   * will get the bribe. */
  if (!diplomat_infiltrate_tile(pplayer, uplayer,
                                pdiplomat, pvictim,
                                pvictim->tile)) {
    return;
  }

  log_debug("bribe-unit: succeeded");

  victim_tile = unit_tile(pvictim);
  pvictim = unit_change_owner(pvictim, pplayer, pdiplomat->homecity,
                              ULR_BRIBED);

  /* N.B.: unit_link always returns the same pointer. As unit_change_owner()
   * currently remove the old unit and replace by a new one (with a new id),
   * we want to make link to the new unit. */
  sz_strlcpy(victim_link, unit_link(pvictim));

  /* Notify everybody involved. */
  notify_player(pplayer, victim_tile, E_MY_DIPLOMAT_BRIBE, ftc_server,
                /* TRANS: <diplomat> ... <unit> */
                _("Your %s succeeded in bribing the %s."),
                unit_link(pdiplomat), victim_link);
  if (maybe_make_veteran(pdiplomat)) {
    notify_unit_experience(pdiplomat);
  }
  notify_player(uplayer, victim_tile, E_ENEMY_DIPLOMAT_BRIBE, ftc_server,
                /* TRANS: <unit> ... <Poles> */
                _("Your %s was bribed by the %s."),
                victim_link, nation_plural_for_player(pplayer));

  /* The unit may have been on a tile shared with a city or a unit
   * it no longer can share a tile with. */
  pcity = tile_city(unit_tile(pvictim));
  bounce = ((NULL != pcity && !pplayers_allied(city_owner(pcity), pplayer))
            || 1 < unit_list_size(unit_tile(pvictim)->units));
  if (bounce) {
    bounce_unit(pvictim, TRUE);
  }

  /* This costs! */
  pplayer->economic.gold -= bribe_cost;

  /* This may cause a diplomatic incident */
  action_consequence_success(ACTION_SPY_BRIBE_UNIT, pplayer, uplayer,
                             victim_tile, victim_link);

  if (!unit_is_alive(diplomat_id)) {
    return;
  }

  if (game.server.unitwaittime_extended) {
    /* Counts as an action even if the move to be tried doesn't succeed */
    unit_did_action(pdiplomat);
  }

  /* Try to move the briber onto the victim's square unless the victim has
   * been bounced because it couldn't share tile with a unit or city. */
  if (!bounce
      /* Post bribe move. */
      && !unit_move_handling(pdiplomat, victim_tile, FALSE, TRUE, NULL)
      /* May have died while trying to move. */
      && unit_is_alive(diplomat_id)) {
    pdiplomat->moves_left = 0;
  }
  if (NULL != player_unit_by_number(pplayer, diplomat_id)) {
    send_unit_info(NULL, pdiplomat);
  }

  /* Update clients. */
  send_player_all_c(pplayer, NULL);
}

/****************************************************************************
  Try to steal a technology from an enemy city.
  If act_id is ACTION_SPY_STEAL_TECH, steal a random technology.
  Otherwise, steal the technology whose ID is "technology".
  (Note: Only Spies can select what to steal.)

  - Check for infiltration success.  Our thief may not survive this.
  - Check for basic success.  Again, our thief may not survive this.
  - If a technology has already been stolen from this city at any time:
    - Diplomats will be caught and executed.
    - Spies will have an increasing chance of being caught and executed.
  - Steal technology!

  - The thief may be captured and executed, or escape to its home town.

  FIXME: It should give a loss of reputation to steal from a player you are
  not at war with
****************************************************************************/
void diplomat_get_tech(struct player *pplayer, struct unit *pdiplomat, 
                       struct city  *pcity, Tech_type_id technology,
                       const enum gen_action act_id)
{
  struct research *presearch, *cresearch;
  struct player *cplayer;
  int count;
  Tech_type_id tech_stolen;

  /* We have to check arguments. They are sent to us by a client,
     so we cannot trust them */
  fc_assert_ret(pcity);
  cplayer = city_owner(pcity);
  fc_assert_ret(cplayer);

  /* Sanity check: The actor still exists. */
  fc_assert_ret(pplayer);
  fc_assert_ret(pdiplomat);

  /* Sanity check: The target is foreign. */
  if (cplayer == pplayer) {
    /* Nothing to do to a domestic target. */
    return;
  }

  if (act_id == ACTION_SPY_STEAL_TECH) {
    /* Can't choose target. Will steal a random tech. */
    technology = A_UNSET;
  }
  
  /* Targeted technology should be a ruleset defined tech,
   * "At Spy's Discretion" (A_UNSET) or a future tech (A_FUTURE). */
  if (technology == A_NONE
      || (technology != A_FUTURE
          && !(technology == A_UNSET && act_id == ACTION_SPY_STEAL_TECH)
          && !valid_advance_by_number(technology))) {
    return;
  }

  presearch = research_get(pplayer);
  cresearch = research_get(cplayer);

  if (technology == A_FUTURE) {
    if (presearch->future_tech >= cresearch->future_tech) {
      return;
    }
  } else if (technology != A_UNSET) {
    if (research_invention_state(presearch, technology) == TECH_KNOWN) {
      return;
    }
    if (research_invention_state(cresearch, technology) != TECH_KNOWN) {
      return;
    }
    if (!research_invention_gettable(presearch, technology,
                                     game.info.tech_steal_allow_holes)) {
      return;
    }
  }

  log_debug("steal-tech: unit: %d", pdiplomat->id);

  /* Check if the Diplomat/Spy succeeds against defending Diplomats/Spies. */
  if (!diplomat_infiltrate_tile(pplayer, cplayer, pdiplomat, NULL,
                                pcity->tile)) {
    return;
  }

  log_debug("steal-tech: infiltrated");

  /* Check if the Diplomat/Spy succeeds with his/her task. */
  /* (Twice as difficult if target is specified.) */
  /* (If already stolen from, impossible for Diplomats and harder for Spies.) */
  if (pcity->server.steal > 0 && !unit_has_type_flag(pdiplomat, UTYF_SPY)) {
    /* Already stolen from: Diplomat always fails! */
    count = 1;
    log_debug("steal-tech: difficulty: impossible");
  } else {
    /* Determine difficulty. */
    count = 1;
    if (act_id == ACTION_SPY_TARGETED_STEAL_TECH) {
      /* Targeted steal tech is more difficult. */
      count++;
    }
    count += pcity->server.steal;
    log_debug("steal-tech: difficulty: %d", count);
    /* Determine success or failure. */
    while (count > 0) {
      if (fc_rand (100) >= game.server.diplchance_steal) {
        break;
      }
      count--;
    }
  }
  
  if (count > 0) {
    if (pcity->server.steal > 0 && !unit_has_type_flag(pdiplomat, UTYF_SPY)) {
      notify_player(pplayer, city_tile(pcity),
                    E_MY_DIPLOMAT_FAILED, ftc_server,
                    /* TRANS: Paris was expecting ... Your Spy was caught */
                    _("%s was expecting your attempt to steal technology "
                      "again. Your %s was caught and executed."),
                    city_link(pcity),
                    unit_tile_link(pdiplomat));
      notify_player(cplayer, city_tile(pcity),
                    E_ENEMY_DIPLOMAT_FAILED, ftc_server,
                    /* TRANS: The Belgian Spy ... from Paris */
                    _("The %s %s failed to steal technology again from %s. "
                      "We were prepared for the attempt."),
                    nation_adjective_for_player(pplayer),
                    unit_tile_link(pdiplomat),
                    city_link(pcity));
    } else {
      notify_player(pplayer, city_tile(pcity),
                    E_MY_DIPLOMAT_FAILED, ftc_server,
                    /* TRANS: Your Spy was caught ... from %s. */
                    _("Your %s was caught in the attempt of"
                      " stealing technology from %s."),
                    unit_tile_link(pdiplomat),
                    city_link(pcity));
      notify_player(cplayer, city_tile(pcity),
                    E_ENEMY_DIPLOMAT_FAILED, ftc_server,
                    /* TRANS: The Belgian Spy ... from Paris */
                    _("The %s %s failed to steal technology from %s."),
                    nation_adjective_for_player(pplayer),
                    unit_tile_link(pdiplomat),
                    city_link(pcity));
    }

    /* This may cause a diplomatic incident */
    action_consequence_caught(act_id, pplayer, cplayer,
                              city_tile(pcity), city_link(pcity));
    wipe_unit(pdiplomat, ULR_CAUGHT, cplayer);
    return;
  } 

  tech_stolen = steal_a_tech(pplayer, cplayer, technology);

  if (tech_stolen == A_NONE) {
    notify_player(pplayer, city_tile(pcity),
                  E_MY_DIPLOMAT_FAILED, ftc_server,
                  _("No new technology found in %s."),
                  city_link(pcity));
    diplomat_charge_movement (pdiplomat, pcity->tile);
    send_unit_info(NULL, pdiplomat);
    return;
  }

  /* Update stealing player's science progress and research fields */
  send_player_all_c(pplayer, NULL);

  /* Record the theft. */
  (pcity->server.steal)++;

  /* this may cause a diplomatic incident */
  action_consequence_success(act_id, pplayer, cplayer,
                             city_tile(pcity), city_link(pcity));

  /* Check if a spy survives her mission. Diplomats never do. */
  diplomat_escape(pplayer, pdiplomat, pcity);
}

/**************************************************************************
  Incite a city to disaffect.

  - Can't incite a city to disaffect if:
    - Player doesn't have enough gold.
  - Check for infiltration success.  Our provocateur may not survive this.
  - Check for basic success.  Again, our provocateur may not survive this.
  - Otherwise, the city will disaffect:
    - Player gets the city.
    - Player gets certain of the city's present/supported units.
    - Player gets a technology advance, if any were unknown.

  - The provocateur may be captured and executed, or escape to its home town.
**************************************************************************/
void diplomat_incite(struct player *pplayer, struct unit *pdiplomat,
		     struct city *pcity)
{
  struct player *cplayer;
  struct tile *ctile;
  const char *clink;
  int revolt_cost;

  /* Fetch target civilization's player.  Sanity checks. */
  fc_assert_ret(pcity);
  cplayer = city_owner(pcity);
  fc_assert_ret(cplayer);

  /* Sanity check: The actor still exists. */
  fc_assert_ret(pplayer);
  fc_assert_ret(pdiplomat);

  /* Sanity check: The target is foreign. */
  if (cplayer == pplayer) {
    /* Nothing to do to a domestic target. */
    return;
  }

  ctile = city_tile(pcity);
  clink = city_link(pcity);

  log_debug("incite: unit: %d", pdiplomat->id);

  /* Get incite cost, ignoring any previously saved value. */
  revolt_cost = city_incite_cost(pplayer, pcity);

  /* If player doesn't have enough gold, can't incite a revolt. */
  if (pplayer->economic.gold < revolt_cost) {
    notify_player(pplayer, ctile, E_MY_DIPLOMAT_FAILED, ftc_server,
                  _("You don't have enough gold to subvert %s."),
                  clink);
    log_debug("incite: not enough gold");
    return;
  }

  /* Check if the Diplomat/Spy succeeds against defending Diplomats/Spies. */
  if (!diplomat_infiltrate_tile(pplayer, cplayer, pdiplomat, NULL,
                                pcity->tile)) {
    return;
  }

  log_debug("incite: infiltrated");

  /* Check if the Diplomat/Spy succeeds with his/her task. */
  if (fc_rand (100) >= game.server.diplchance) {
    notify_player(pplayer, ctile, E_MY_DIPLOMAT_FAILED, ftc_server,
                  _("Your %s was caught in the attempt"
                    " of inciting a revolt!"),
                  unit_tile_link(pdiplomat));
    notify_player(cplayer, ctile, E_ENEMY_DIPLOMAT_FAILED, ftc_server,
                  _("You caught %s %s attempting"
                    " to incite a revolt in %s!"),
                  nation_adjective_for_player(pplayer),
                  unit_tile_link(pdiplomat),
                  clink);

    if (game.server.incite_gold_recovered != -1) {
      pplayer->economic.gold -= revolt_cost;
      cplayer->economic.gold += revolt_cost
                                * game.server.incite_gold_recovered / 100;
    }

    /* This may cause a diplomatic incident */
    action_consequence_caught(ACTION_SPY_INCITE_CITY,
                              pplayer, cplayer, ctile, clink);

    wipe_unit(pdiplomat, ULR_CAUGHT, cplayer);
    return;
  }

  log_debug("incite: succeeded");

  /* Subvert the city to your cause... */

  /* City loses some population. */
  if (city_size_get(pcity) > 1) {
    city_reduce_size(pcity, 1, pplayer, "incited");
  }

  /* This costs! */
  pplayer->economic.gold -= revolt_cost;

  /* Notify everybody involved. */
  notify_player(pplayer, ctile, E_MY_DIPLOMAT_INCITE, ftc_server,
                _("Revolt incited in %s, you now rule the city!"),
                clink);
  notify_player(cplayer, ctile, E_ENEMY_DIPLOMAT_INCITE, ftc_server,
                _("%s has revolted, %s influence suspected."),
                clink,
                nation_adjective_for_player(pplayer));

  pcity->shield_stock = 0;
  nullify_prechange_production(pcity);

  /* this may cause a diplomatic incident */
  action_consequence_success(ACTION_SPY_INCITE_CITY,
                             pplayer, cplayer, ctile, clink);

  /* Transfer city and units supported by this city (that
     are within one square of the city) to the new owner. */
  if (transfer_city(pplayer, pcity, 1, TRUE, TRUE, FALSE,
                    !is_barbarian(pplayer))) {
    script_server_signal_emit("city_transferred", pcity, cplayer, pplayer,
                              "incited");
  }

  /* Check if a spy survives her mission. Diplomats never do.
   * _After_ transferring the city, or the city area is first fogged
   * when the diplomat is removed, and then unfogged when the city
   * is transferred. */
  diplomat_escape_full(pplayer, pdiplomat, TRUE, ctile, clink);

  /* Update the players gold in the client */
  send_player_info_c(pplayer, pplayer->connections);
}

/**************************************************************************
  Sabotage enemy city's improvement or production.
  If this is untargeted sabotage city a random improvement or production is
  targeted.
  Targeted sabotage city lets the value of "improvement" decide the target.
  If "improvement" is -1, sabotage current production.
  Otherwise, sabotage the city improvement whose ID is "improvement".

  - Check for infiltration success.  Our saboteur may not survive this.
  - Check for basic success.  Again, our saboteur may not survive this.
  - Determine target, given arguments and constraints.
  - If specified, city walls and anything in a capital are 50% likely to fail.
  - Do sabotage!

  - The saboteur may be captured and executed, or escape to its home town.
**************************************************************************/
void diplomat_sabotage(struct player *pplayer, struct unit *pdiplomat,
                       struct city *pcity, Impr_type_id improvement,
                       const enum gen_action act_id)
{
  struct player *cplayer;
  struct impr_type *ptarget;
  int count, which;
  int success_prob;

  /* Fetch target city's player.  Sanity checks. */
  fc_assert_ret(pcity);
  cplayer = city_owner(pcity);
  fc_assert_ret(cplayer);

  /* Sanity check: The actor still exists. */
  fc_assert_ret(pplayer);
  fc_assert_ret(pdiplomat);

  log_debug("sabotage: unit: %d", pdiplomat->id);

  /* Twice as difficult if target is specified. */
  success_prob = (improvement >= B_LAST ? game.server.diplchance 
                  : game.server.diplchance / 2); 

  /* Check if the Diplomat/Spy succeeds against defending Diplomats/Spies. */
  if (!diplomat_infiltrate_tile(pplayer, cplayer, pdiplomat, NULL,
                                pcity->tile)) {
    return;
  }

  log_debug("sabotage: infiltrated");

  /* Check if the Diplomat/Spy succeeds with his/her task. */
  if (fc_rand (100) >= success_prob) {
    notify_player(pplayer, city_tile(pcity),
                  E_MY_DIPLOMAT_FAILED, ftc_server,
                  _("Your %s was caught in the attempt"
                    " of industrial sabotage!"),
                  unit_tile_link(pdiplomat));
    notify_player(cplayer, city_tile(pcity),
                  E_ENEMY_DIPLOMAT_SABOTAGE, ftc_server,
                  _("You caught %s %s attempting sabotage in %s!"),
                  nation_adjective_for_player(pplayer),
                  unit_tile_link(pdiplomat),
                  city_link(pcity));
    wipe_unit(pdiplomat, ULR_CAUGHT, cplayer);
    return;
  }

  log_debug("sabotage: succeeded");

  /* Examine the city for improvements to sabotage. */
  count = 0;
  city_built_iterate(pcity, pimprove) {
    if (pimprove->sabotage > 0) {
      count++;
    }
  } city_built_iterate_end;

  log_debug("sabotage: count of improvements: %d", count);

  /* Determine the target (-1 is production). */
  if (act_id == ACTION_SPY_SABOTAGE_CITY) {
    /*
     * Pick random:
     * 50/50 chance to pick production or some improvement.
     * Won't pick something that doesn't exit.
     * If nothing to do, say so, deduct movement cost and return.
     */
    if (count == 0 && pcity->shield_stock == 0) {
      notify_player(pplayer, city_tile(pcity),
                    E_MY_DIPLOMAT_FAILED, ftc_server,
                    _("Your %s could not find anything to sabotage in %s."),
                    unit_link(pdiplomat),
                    city_link(pcity));
      diplomat_charge_movement(pdiplomat, pcity->tile);
      send_unit_info(NULL, pdiplomat);
      log_debug("sabotage: random: nothing to do");
      return;
    }
    if (count == 0 || fc_rand (2) == 1) {
      ptarget = NULL;
      log_debug("sabotage: random: targeted production");
    } else {
      ptarget = NULL;
      which = fc_rand (count);

      city_built_iterate(pcity, pimprove) {
	if (pimprove->sabotage > 0) {
	  if (which > 0) {
	    which--;
	  } else {
	    ptarget = pimprove;
	    break;
	  }
	}
      } city_built_iterate_end;

      if (NULL != ptarget) {
        log_debug("sabotage: random: targeted improvement: %d (%s)",
                  improvement_number(ptarget),
                  improvement_rule_name(ptarget));
      } else {
        log_error("sabotage: random: targeted improvement error!");
      }
    }
  } else if (improvement < 0) {
    /* If told to sabotage production, do so. */
    ptarget = NULL;
    log_debug("sabotage: specified target production");
  } else {
    struct impr_type *pimprove = improvement_by_number(improvement);
    if (pimprove == NULL) {
      log_error("sabotage: requested for invalid improvement %d", improvement);
      return;
    }
    /*
     * Told which improvement to pick:
     * If try for wonder or palace, complain, deduct movement cost and return.
     * If not available, say so, deduct movement cost and return.
     */
    if (city_has_building(pcity, pimprove)) {
      if (pimprove->sabotage > 0) {
	ptarget = pimprove;
        log_debug("sabotage: specified target improvement: %d (%s)",
                  improvement, improvement_rule_name(pimprove));
      } else {
        notify_player(pplayer, city_tile(pcity),
                      E_MY_DIPLOMAT_FAILED, ftc_server,
                      _("You cannot sabotage a %s!"),
                      improvement_name_translation(pimprove));
        diplomat_charge_movement(pdiplomat, pcity->tile);
        send_unit_info(NULL, pdiplomat);
        log_debug("sabotage: disallowed target improvement: %d (%s)",
                  improvement, improvement_rule_name(pimprove));
        return;
      }
    } else {
      notify_player(pplayer, city_tile(pcity),
                    E_MY_DIPLOMAT_FAILED, ftc_server,
                    _("Your %s could not find the %s to sabotage in %s."),
                    unit_name_translation(pdiplomat),
                    improvement_name_translation(pimprove),
                    city_link(pcity));
      diplomat_charge_movement(pdiplomat, pcity->tile);
      send_unit_info(NULL, pdiplomat);
      log_debug("sabotage: target improvement not found: %d (%s)",
                improvement, improvement_rule_name(pimprove));
      return;
    }
  }

  /* Now, the fun stuff!  Do the sabotage! */
  if (NULL == ptarget) {
     char prod[256];

    /* Do it. */
    pcity->shield_stock = 0;
    nullify_prechange_production(pcity); /* Make it impossible to recover */

    /* Report it. */
    universal_name_translation(&pcity->production, prod, sizeof(prod));

    notify_player(pplayer, city_tile(pcity),
                  E_MY_DIPLOMAT_SABOTAGE, ftc_server,
                  _("Your %s succeeded in destroying"
                    " the production of %s in %s."),
                  unit_link(pdiplomat),
                  prod,
                  city_name_get(pcity));
    notify_player(cplayer, city_tile(pcity),
                  E_ENEMY_DIPLOMAT_SABOTAGE, ftc_server,
                  _("The production of %s was destroyed in %s,"
                    " %s are suspected."),
                  prod,
                  city_link(pcity),
                  nation_plural_for_player(pplayer));
    log_debug("sabotage: sabotaged production");
  } else {
    int vulnerability = ptarget->sabotage;

    /* Sabotage a city improvement. */

    /*
     * One last chance to get caught:
     * If target was specified, and it is in the capital or are
     * City Walls, then there is a 50% chance of getting caught.
     */
    vulnerability -= (vulnerability
		      * get_city_bonus(pcity, EFT_SPY_RESISTANT) / 100);

    if (fc_rand(100) >= vulnerability) {
      /* Caught! */
      notify_player(pplayer, city_tile(pcity),
                    E_MY_DIPLOMAT_FAILED, ftc_server,
                    _("Your %s was caught in the attempt of sabotage!"),
                    unit_tile_link(pdiplomat));
      notify_player(cplayer, city_tile(pcity),
                    E_ENEMY_DIPLOMAT_FAILED, ftc_server,
                    _("You caught %s %s attempting"
                      " to sabotage the %s in %s!"),
                    nation_adjective_for_player(pplayer),
                    unit_tile_link(pdiplomat),
                    improvement_name_translation(ptarget),
                    city_link(pcity));
      wipe_unit(pdiplomat, ULR_CAUGHT, cplayer);
      log_debug("sabotage: caught in capital or on city walls");
      return;
    }

    /* Report it. */
    notify_player(pplayer, city_tile(pcity),
                  E_MY_DIPLOMAT_SABOTAGE, ftc_server,
                  _("Your %s destroyed the %s in %s."),
                  unit_link(pdiplomat),
                  improvement_name_translation(ptarget),
                  city_link(pcity));
    notify_player(cplayer, city_tile(pcity),
                  E_ENEMY_DIPLOMAT_SABOTAGE, ftc_server,
                  _("The %s destroyed the %s in %s."),
                  nation_plural_for_player(pplayer),
                  improvement_name_translation(ptarget),
                  city_link(pcity));
    log_debug("sabotage: sabotaged improvement: %d (%s)",
              improvement_number(ptarget),
              improvement_rule_name(ptarget));

    /* Do it. */
    building_lost(pcity, ptarget);
  }

  /* Update clients. */
  send_city_info(NULL, pcity);

  /* this may cause a diplomatic incident */
  action_consequence_success(act_id, pplayer, cplayer,
                             city_tile(pcity), city_link(pcity));

  /* Check if a spy survives her mission. Diplomats never do. */
  diplomat_escape(pplayer, pdiplomat, pcity);
}

/**************************************************************************
  Steal gold from another player.
  The amount stolen is decided randomly.
  Not everything stolen reaches the player that ordered it stolen.

  - Check for infiltration success.  Our thief may not survive this.
  - Check for basic success.  Again, our thief may not survive this.
  - Can't steal if there is no money to take.
**************************************************************************/
void spy_steal_gold(struct player *act_player, struct unit *act_unit,
                    struct city *tgt_city)
{
  struct player *tgt_player;
  struct tile *tgt_tile;

  const char *tgt_city_link;

  int gold_take;
  int gold_give;

  /* Sanity check: The actor still exists. */
  fc_assert_ret(act_player);
  fc_assert_ret(act_unit);

  /* Sanity check: The target city still exists. */
  fc_assert_ret(tgt_city);

  /* Who to steal from. */
  tgt_player = city_owner(tgt_city);

  /* Sanity check: The target player still exists. */
  fc_assert_ret(tgt_player);

  /* Sanity check: The target is foreign. */
  if (tgt_player == act_player) {
    /* Nothing to do to a domestic target. */
    return;
  }

  /* Sanity check: There is something to steal. */
  if (tgt_player->economic.gold <= 0) {
    return;
  }

  tgt_tile = city_tile(tgt_city);
  tgt_city_link = city_link(tgt_city);

  log_debug("steal gold: unit: %d", act_unit->id);

  /* Battle all units capable of diplomatic defence. */
  if (!diplomat_infiltrate_tile(act_player, tgt_player,
                                act_unit, NULL, tgt_tile)) {
    return;
  }

  log_debug("steal gold: infiltrated");

  /* The thief may get caught while trying to steal the gold. */
  if (fc_rand (100) >= game.server.diplchance) {
    notify_player(act_player, tgt_tile, E_MY_DIPLOMAT_FAILED, ftc_server,
                  _("Your %s was caught attempting to steal gold!"),
                  unit_tile_link(act_unit));
    notify_player(tgt_player, tgt_tile, E_ENEMY_DIPLOMAT_FAILED,
                  ftc_server,
                  /* TRANS: nation, unit, city */
                  _("You caught %s %s attempting"
                    " to steal your gold in %s!"),
                  nation_adjective_for_player(act_player),
                  unit_tile_link(act_unit),
                  tgt_city_link);

    /* Execute the caught thief. */
    wipe_unit(act_unit, ULR_CAUGHT, tgt_player);

    return;
  }

  log_debug("steal gold: succeeded");

  /* The upper limit on how much gold the thief can steal. */
  gold_take = (tgt_player->economic.gold
               * get_city_bonus(tgt_city, EFT_MAX_STOLEN_GOLD_PM))
              / 1000;

  /* How much to actually take. 1 gold is the smallest amount that can be
   * stolen. The victim player has at least 1 gold. If he didn't the
   * something to steal sanity check would have aborted the theft. */
  gold_take = fc_rand(gold_take) + 1;

  log_debug("steal gold: will take %d gold", gold_take);

  /* Steal the gold. */
  tgt_player->economic.gold -= gold_take;

  /* Some gold may be lost during transfer. */
  gold_give = gold_take
            - (gold_take * get_unit_bonus(act_unit, EFT_THIEFS_SHARE_PM))
              / 1000;

  log_debug("steal gold: will give %d gold", gold_give);

  /* Pocket the stolen money. */
  act_player->economic.gold += gold_give;

  /* Notify everyone involved. */
  notify_player(act_player, tgt_tile, E_MY_SPY_STEAL_GOLD, ftc_server,
                /* TRANS: unit, gold, city */
                PL_("Your %s stole %d gold from %s.",
                    "Your %s stole %d gold from %s.", gold_give),
                unit_link(act_unit), gold_give, tgt_city_link);
  notify_player(tgt_player, tgt_tile, E_ENEMY_SPY_STEAL_GOLD, ftc_server,
                /* TRANS: gold, city, nation */
                PL_("%d gold stolen from %s, %s suspected.",
                    "%d gold stolen from %s, %s suspected.", gold_take),
                gold_take, tgt_city_link,
                nation_plural_for_player(act_player));

  /* This may cause a diplomatic incident. */
  action_consequence_success(ACTION_SPY_STEAL_GOLD, act_player,
                             tgt_player, tgt_tile, tgt_city_link);

  /* Try to escape. */
  diplomat_escape_full(act_player, act_unit, TRUE,
                       tgt_tile, tgt_city_link);

  /* Update the players' gold in the client */
  send_player_info_c(act_player, act_player->connections);
  send_player_info_c(tgt_player, tgt_player->connections);
}

/**************************************************************************
  This subtracts the destination movement cost from a diplomat/spy.
**************************************************************************/
static void diplomat_charge_movement (struct unit *pdiplomat, struct tile *ptile)
{
  pdiplomat->moves_left -=
    map_move_cost_unit(pdiplomat, ptile);
  if (pdiplomat->moves_left < 0) {
    pdiplomat->moves_left = 0;
  }
}

/**************************************************************************
  This determines if a diplomat/spy succeeds against some defender,
  who is also a diplomat or spy. Note: a superspy defender always
  succeeds, otherwise a superspy attacker always wins.

  Return TRUE if the "attacker" succeeds.
**************************************************************************/
static bool diplomat_success_vs_defender(struct unit *pattacker,
                                         struct unit *pdefender,
                                         struct tile *pdefender_tile)
{
  int chance = 50; /* Base 50% chance */

  if (unit_has_type_flag(pdefender, UTYF_SUPERSPY)) {
    /* A defending UTYF_SUPERSPY will defeat every possible attacker. */
    return FALSE;
  }
  if (unit_has_type_flag(pattacker, UTYF_SUPERSPY)) {
    /* An attacking UTYF_SUPERSPY will defeat every possible defender
     * except another UTYF_SUPERSPY. */
    return TRUE;
  }

  /* Add or remove 25% if spy flag. */
  if (unit_has_type_flag(pattacker, UTYF_SPY)) {
    chance += 25;
  }
  if (unit_has_type_flag(pdefender, UTYF_SPY)) {
    chance -= 25;
  }

  /* Use power_fact from veteran level to modify chance in a linear way.
   * Equal veteran levels cancel out.
   * It's probably not good for rulesets to allow this to have more than
   * 20% effect. */
  {
    const struct veteran_level
      *vatt = utype_veteran_level(unit_type_get(pattacker), pattacker->veteran);
    const struct veteran_level
      *vdef = utype_veteran_level(unit_type_get(pdefender), pdefender->veteran);
    fc_assert_ret_val(vatt != NULL && vdef != NULL, FALSE);
    chance += vatt->power_fact - vdef->power_fact;
  }

  /* Reduce the chance of an attack if BF_DIPLOMAT_DEFENSE is active. */
  if (tile_has_base_flag_for_unit(pdefender_tile, unit_type_get(pdefender),
                                  BF_DIPLOMAT_DEFENSE)) {
    chance -= chance * 25 / 100; /* 25% penalty */
  }

  if (tile_city(pdefender_tile)) {
    /* Reduce the chance of an attack by EFT_SPY_RESISTANT percent. */
    chance -= chance * get_city_bonus(tile_city(pdefender_tile),
                                      EFT_SPY_RESISTANT) / 100;
  }

  return (int)fc_rand(100) < chance;
}

/**************************************************************************
  This determines if a diplomat/spy succeeds in infiltrating a tile.

  - The infiltrator must go up against each defender.
  - The victim unit won't defend. (NULL if everyone should defend)
  - One or the other is eliminated in each contest.

  - Return TRUE if the infiltrator succeeds.

  'pplayer' is the player who tries to do a spy/diplomat action on 'ptile'
  with the unit 'pdiplomat' against 'cplayer'.
**************************************************************************/
static bool diplomat_infiltrate_tile(struct player *pplayer,
                                     struct player *cplayer,
                                     struct unit *pdiplomat,
                                     struct unit *pvictim,
                                     struct tile *ptile)
{
  char link_city[MAX_LEN_LINK] = "";
  char link_diplomat[MAX_LEN_LINK];
  char link_unit[MAX_LEN_LINK];
  struct city *pcity = tile_city(ptile);

  if (pcity) {
    /* N.B.: *_link() always returns the same pointer. */
    sz_strlcpy(link_city, city_link(pcity));
  }

  /* We don't need a _safe iterate since no transporters should be
   * destroyed. */
  unit_list_iterate(ptile->units, punit) {
    struct player *uplayer = unit_owner(punit);

    /* I can't confirm if we won't deny that we weren't involved. */
    if (uplayer == pplayer) {
      continue;
    }

    if (punit == pvictim
        && !unit_has_type_flag(punit, UTYF_SUPERSPY)) {
      /* The victim unit is defenseless unless it's a SuperSpy.
       * Rationalization: A regular diplomat don't mind being bribed. A
       * SuperSpy is high enough up the chain that accepting a bribe is
       * against his own interests. */
      continue;
    }

    if (unit_has_type_flag(punit, UTYF_DIPLOMAT)
        || unit_has_type_flag(punit, UTYF_SUPERSPY)) {
      /* A UTYF_SUPERSPY unit may not actually be a spy, but a superboss
       * which we cannot allow puny diplomats from getting the better
       * of. UTYF_SUPERSPY vs UTYF_SUPERSPY in a diplomatic contest always
       * kills the attacker. */

      if (diplomat_success_vs_defender(pdiplomat, punit, ptile)) {
        /* Defending Spy/Diplomat dies. */

        /* N.B.: *_link() always returns the same pointer. */
        sz_strlcpy(link_unit, unit_tile_link(punit));
        sz_strlcpy(link_diplomat, unit_link(pdiplomat));

        notify_player(pplayer, ptile, E_ENEMY_DIPLOMAT_FAILED, ftc_server,
                      /* TRANS: <unit> ... <diplomat> */
                      _("An enemy %s has been eliminated by your %s."),
                      link_unit, link_diplomat);

        if (pcity) {
          if (uplayer == cplayer) {
            notify_player(cplayer, ptile, E_MY_DIPLOMAT_FAILED, ftc_server,
                          /* TRANS: <unit> ... <city> ... <diplomat> */
                          _("Your %s has been eliminated defending %s"
                            " against a %s."), link_unit, link_city,
                          link_diplomat);
          } else {
            notify_player(cplayer, ptile, E_MY_DIPLOMAT_FAILED, ftc_server,
                          /* TRANS: <nation adj> <unit> ... <city>
                           * TRANS: ... <diplomat> */
                          _("A %s %s has been eliminated defending %s "
                            "against a %s."),
                          nation_adjective_for_player(uplayer),
                          link_unit, link_city, link_diplomat);
            notify_player(uplayer, ptile, E_MY_DIPLOMAT_FAILED, ftc_server,
                          /* TRANS: ... <unit> ... <nation adj> <city>
                           * TRANS: ... <diplomat> */
                          _("Your %s has been eliminated defending %s %s "
                            "against a %s."), link_unit,
                          nation_adjective_for_player(cplayer),
                          link_city, link_diplomat);
          }
        } else {
          if (uplayer == cplayer) {
            notify_player(cplayer, ptile, E_MY_DIPLOMAT_FAILED, ftc_server,
                          /* TRANS: <unit> ... <diplomat> */
                          _("Your %s has been eliminated defending "
                            "against a %s."), link_unit, link_diplomat);
          } else {
            notify_player(cplayer, ptile, E_MY_DIPLOMAT_FAILED, ftc_server,
                          /* TRANS: <nation adj> <unit> ... <diplomat> */
                          _("A %s %s has been eliminated defending "
                            "against a %s."),
                          nation_adjective_for_player(uplayer),
                          link_unit, link_diplomat);
            notify_player(uplayer, ptile, E_MY_DIPLOMAT_FAILED, ftc_server,
                          /* TRANS: ... <unit> ... <diplomat> */
                          _("Your %s has been eliminated defending "
                            "against a %s."), link_unit, link_diplomat);
          }
        }

        pdiplomat->moves_left = MAX(0, pdiplomat->moves_left - SINGLE_MOVE);

        /* Attacking unit became more experienced? */
        if (maybe_make_veteran(pdiplomat)) {
          notify_unit_experience(pdiplomat);
        }
        send_unit_info(NULL, pdiplomat);
        wipe_unit(punit, ULR_ELIMINATED, pplayer);
        return FALSE;
      } else {
        /* Attacking Spy/Diplomat dies. */

        /* N.B.: *_link() always returns the same pointer. */
        sz_strlcpy(link_unit, unit_link(punit));
        sz_strlcpy(link_diplomat, unit_tile_link(pdiplomat));

        notify_player(pplayer, ptile, E_MY_DIPLOMAT_FAILED, ftc_server,
                      _("Your %s was eliminated by a defending %s."),
                      link_diplomat, link_unit);

        if (pcity) {
          if (uplayer == cplayer) {
            notify_player(cplayer, ptile, E_ENEMY_DIPLOMAT_FAILED, ftc_server,
                          _("Eliminated a %s %s while infiltrating %s."),
                          nation_adjective_for_player(pplayer),
                          link_diplomat, link_city);
          } else {
            notify_player(cplayer, ptile, E_ENEMY_DIPLOMAT_FAILED, ftc_server,
                          _("A %s %s eliminated a %s %s while infiltrating "
                            "%s."), nation_adjective_for_player(uplayer),
                          link_unit, nation_adjective_for_player(pplayer),
                          link_diplomat, link_city);
            notify_player(uplayer, ptile, E_ENEMY_DIPLOMAT_FAILED, ftc_server,
                          _("Your %s eliminated a %s %s while infiltrating "
                            "%s."), link_unit,
                          nation_adjective_for_player(pplayer),
                          link_diplomat, link_city);
          }
        } else {
          if (uplayer == cplayer) {
            notify_player(cplayer, ptile, E_ENEMY_DIPLOMAT_FAILED, ftc_server,
                          _("Eliminated a %s %s while infiltrating our troops."),
                          nation_adjective_for_player(pplayer),
                          link_diplomat);
          } else {
            notify_player(cplayer, ptile, E_ENEMY_DIPLOMAT_FAILED, ftc_server,
                          _("A %s %s eliminated a %s %s while infiltrating our "
                            "troops."), nation_adjective_for_player(uplayer),
                          link_unit, nation_adjective_for_player(pplayer),
                          link_diplomat);
            notify_player(uplayer, ptile, E_ENEMY_DIPLOMAT_FAILED, ftc_server,
                          /* TRANS: ... <unit> ... <diplomat> */
                          _("Your %s eliminated a %s %s while infiltrating our "
                            "troops."), link_unit,
                          nation_adjective_for_player(pplayer),
                          link_diplomat);
          }
        }

	/* Defending unit became more experienced? */
	if (maybe_make_veteran(punit)) {
	  notify_unit_experience(punit);
	}
        wipe_unit(pdiplomat, ULR_ELIMINATED, uplayer);
        return FALSE;
      }
    }
  } unit_list_iterate_end;

  return TRUE;
}

/**************************************************************************
  This determines if a diplomat/spy survives and escapes.
  If "pcity" is NULL, assume action was in the field.

  Spies have a game.server.diplchance specified chance of survival (better
  if veteran):
    - Diplomats always die.
    - Escapes to home city.
    - Escapee may become a veteran.
**************************************************************************/
static void diplomat_escape(struct player *pplayer, struct unit *pdiplomat,
                            const struct city *pcity)
{
  struct tile *ptile;
  const char *vlink;

  if (pcity) {
    ptile = city_tile(pcity);
    vlink = city_link(pcity);
  } else {
    ptile = unit_tile(pdiplomat);
    vlink = NULL;
  }

  return diplomat_escape_full(pplayer, pdiplomat, pcity != NULL,
                              ptile, vlink);
}

/**************************************************************************
  This determines if a diplomat/spy survives and escapes.

  Spies have a game.server.diplchance specified chance of survival (better 
  if veteran):
    - Diplomats always die.
    - Escapes to home city.
    - Escapee may become a veteran.
**************************************************************************/
static void diplomat_escape_full(struct player *pplayer,
                                 struct unit *pdiplomat,
                                 bool city_related,
                                 struct tile *ptile,
                                 const char *vlink)
{
  int escapechance;
  struct city *spyhome;

  /* Veteran level's power factor's effect on escape chance is relative to
   * unpromoted unit's power factor */
  {
    const struct veteran_level
      *vunit = utype_veteran_level(unit_type_get(pdiplomat), pdiplomat->veteran);
    const struct veteran_level
      *vbase = utype_veteran_level(unit_type_get(pdiplomat), 0);

    escapechance = game.server.diplchance
      + (vunit->power_fact - vbase->power_fact);
  }

  /* find closest city for escape target */
  spyhome = find_closest_city(ptile, NULL, unit_owner(pdiplomat), FALSE,
                              FALSE, FALSE, TRUE, FALSE, NULL);

  if (spyhome
      && unit_has_type_flag(pdiplomat, UTYF_SPY)
      && (unit_has_type_flag(pdiplomat, UTYF_SUPERSPY)
          || fc_rand (100) < escapechance)) {
    /* Attacking Spy/Diplomat survives. */
    notify_player(pplayer, ptile, E_MY_DIPLOMAT_ESCAPE, ftc_server,
                  _("Your %s has successfully completed"
                    " the mission and returned unharmed to %s."),
                  unit_link(pdiplomat),
                  city_link(spyhome));
    if (maybe_make_veteran(pdiplomat)) {
      notify_unit_experience(pdiplomat);
    }

    /* being teleported costs all movement */
    if (!teleport_unit_to_city (pdiplomat, spyhome, -1, FALSE)) {
      send_unit_info(NULL, pdiplomat);
      log_error("Bug in diplomat_escape: Spy can't teleport.");
      return;
    }

    return;
  } else {
    if (city_related) {
      notify_player(pplayer, ptile, E_MY_DIPLOMAT_FAILED, ftc_server,
                    _("Your %s was captured after completing"
                      " the mission in %s."),
                    unit_tile_link(pdiplomat),
                    vlink);
    } else {
      notify_player(pplayer, ptile, E_MY_DIPLOMAT_FAILED, ftc_server,
                    _("Your %s was captured after completing"
                      " the mission."),
                    unit_tile_link(pdiplomat));
    }
  }

  wipe_unit(pdiplomat,
            /* A non Spy can't escape. It is therefore spent, not caught. */
            unit_has_type_flag(pdiplomat, UTYF_SPY) ? ULR_CAUGHT : ULR_USED,
            NULL);
}

/**************************************************************************
 return number of diplomats on this square.  AJS 20000130
**************************************************************************/
int count_diplomats_on_tile(struct tile *ptile)
{
  int count = 0;

  unit_list_iterate((ptile)->units, punit) {
    if (unit_has_type_flag(punit, UTYF_DIPLOMAT)) {
      count++;
    }
  } unit_list_iterate_end;

  return count;
}
