/********************************************************************** 
 Freeciv - Copyright (C) 2002 - The Freeciv Project
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

/* utility */
#include "log.h"
#include "mem.h"

/* common */
#include "city.h"
#include "effects.h"
#include "game.h"
#include "government.h"
#include "map.h"
#include "movement.h"
#include "research.h"
#include "unit.h"
#include "unitlist.h"

/* common/aicore */
#include "aisupport.h"
#include "path_finding.h"
#include "pf_tools.h"

/* server */
#include "citytools.h"
#include "cityturn.h"
#include "diplhand.h"
#include "maphand.h"
#include "plrhand.h"
#include "srv_log.h"
#include "unittools.h"

/* server/advisors */
#include "advtools.h"
#include "autosettlers.h"

/* ai */
#include "aiplayer.h"
#include "aitools.h"

#include "advdata.h"

/**************************************************************************
  Precalculates some important data about the improvements in the game
  that we use later in ai/aicity.c.  We mark improvements as 'calculate'
  if we want to run a full test on them, as 'estimate' if we just want
  to do some guesses on them, or as 'unused' is they are useless to us.
  Then we find the largest range of calculatable effects in the
  improvement and record it for later use.
**************************************************************************/
static void ai_data_city_impr_calc(struct player *pplayer, struct adv_data *ai)
{
  int count[AI_IMPR_LAST];

  memset(count, 0, sizeof(count));

  improvement_iterate(pimprove) {
    struct universal source = {
      .kind = VUT_IMPROVEMENT,
      .value = {.building = pimprove}
    };

    ai->impr_calc[improvement_index(pimprove)] = AI_IMPR_ESTIMATE;

    /* Find largest extension */
    effect_list_iterate(get_req_source_effects(&source), peffect) {
      switch (peffect->type) {
#if 0
      /* TODO */
      case EFT_FORCE_CONTENT:
      case EFT_FORCE_CONTENT_PCT:
      case EFT_MAKE_CONTENT:
      case EFT_MAKE_CONTENT_MIL:
      case EFT_MAKE_CONTENT_MIL_PER:
      case EFT_MAKE_CONTENT_PCT:
      case EFT_MAKE_HAPPY:
#endif
      case EFT_CAPITAL_CITY:
      case EFT_POLLU_POP_PCT:
      case EFT_POLLU_PROD_PCT:
      case EFT_OUTPUT_BONUS:
      case EFT_OUTPUT_BONUS_2:
      case EFT_OUTPUT_WASTE_PCT:
      case EFT_UPKEEP_FREE:
	requirement_list_iterate(peffect->reqs, preq) {
	  if (VUT_IMPROVEMENT == preq->source.kind
	      && preq->source.value.building == pimprove) {
            if (ai->impr_calc[improvement_index(pimprove)] != AI_IMPR_CALCULATE_FULL) {
	      ai->impr_calc[improvement_index(pimprove)] = AI_IMPR_CALCULATE;
            }
	    if (preq->range > ai->impr_range[improvement_index(pimprove)]) {
	      ai->impr_range[improvement_index(pimprove)] = preq->range;
	    }
	  }
	} requirement_list_iterate_end;
        break;
      case EFT_OUTPUT_ADD_TILE:
      case EFT_OUTPUT_PER_TILE:
      case EFT_OUTPUT_INC_TILE:
	requirement_list_iterate(peffect->reqs, preq) {
	  if (VUT_IMPROVEMENT == preq->source.kind
	      && preq->source.value.building == pimprove) {
	    ai->impr_calc[improvement_index(pimprove)] = AI_IMPR_CALCULATE_FULL;
	    if (preq->range > ai->impr_range[improvement_index(pimprove)]) {
	      ai->impr_range[improvement_index(pimprove)] = preq->range;
	    }
	  }
	} requirement_list_iterate_end;
      break;
      default:
      /* Nothing! */
      break;
      }
    } effect_list_iterate_end;
  } improvement_iterate_end;
}

/**************************************************************************
  Check if the player still takes advantage of EFT_TECH_PARASITE.
  Research is useless if there are still techs which may be given to the
  player for free.
**************************************************************************/
static bool player_has_really_useful_tech_parasite(struct player* pplayer)
{
  int players_needed = get_player_bonus(pplayer, EFT_TECH_PARASITE);

  if (players_needed == 0) {
    return FALSE;
  }
  
  advance_index_iterate(A_FIRST, tech) {
    int players_having;

    if (!player_invention_reachable(pplayer, tech, FALSE)
        || TECH_KNOWN == player_invention_state(pplayer, tech)) {
      continue;
    }

    players_having = 0;

    players_iterate(aplayer) {
      if (aplayer == pplayer || !aplayer->is_alive) {
        continue;
      }

      if (TECH_KNOWN == player_invention_state(aplayer, tech)
          || player_research_get(aplayer)->researching == tech) {
	players_having++;
	if (players_having >= players_needed) {
	  return TRUE;
	}
      }
    } players_iterate_end;
  } advance_index_iterate_end;
  return FALSE;
}

/**************************************************************************
  Analyze rulesets. Must be run after rulesets are loaded, unlike
  _init, which must be run before savegames are loaded, which is usually
  before rulesets.
**************************************************************************/
void ai_data_analyze_rulesets(struct player *pplayer)
{
  struct adv_data *ai = pplayer->server.adv;

  fc_assert_ret(ai != NULL);

  ai_data_city_impr_calc(pplayer, ai);
}

/**************************************************************************
  This function is called each turn to initialize pplayer->ai.stats.units.
**************************************************************************/
static void count_my_units(struct player *pplayer)
{
  struct adv_data *ai = adv_data_get(pplayer);

  memset(&ai->stats.units, 0, sizeof(ai->stats.units));

  unit_list_iterate(pplayer->units, punit) {
    struct unit_class *pclass = unit_class(punit);

    if (pclass->ai.land_move != MOVE_NONE
        && pclass->ai.sea_move != MOVE_NONE) {
      /* Can move both land and ocean */
      ai->stats.units.amphibious++;
    } else if (pclass->ai.land_move != MOVE_NONE) {
      /* Can move only at land */
      ai->stats.units.land++;
    } else if (pclass->ai.sea_move != MOVE_NONE) {
      /* Can move only at sea */
      ai->stats.units.sea++;
    }

    if (unit_has_type_flag(punit, F_TRIREME)) {
      ai->stats.units.triremes++;
    }
    if (uclass_has_flag(unit_class(punit), UCF_MISSILE)) {
      ai->stats.units.missiles++;
    }
    if (unit_has_type_flag(punit, F_PARATROOPERS)) {
      ai->stats.units.paratroopers++;
    }
    if (can_upgrade_unittype(pplayer, unit_type(punit)) >= 0) {
      ai->stats.units.upgradeable++;
    }
  } unit_list_iterate_end;
}

/**************************************************************************
  Return whether data phase is currently open. Data phase is open
  between adv_data_phase_init() and adv_data_phase_done() calls.
**************************************************************************/
bool is_adv_data_phase_open(struct player *pplayer)
{
  struct adv_data *adv = pplayer->server.adv;

  return adv->phase_is_initialized;
}

/**************************************************************************
  Make and cache lots of calculations needed for other functions.

  Returns TRUE if new data was created, FALSE if data existed already.

  Note: We use map.num_continents here rather than pplayer->num_continents
  because we are omniscient and don't care about such trivialities as who
  can see what.

  FIXME: We should try to find the lowest common defence strength of our
  defending units, and ignore enemy units that are incapable of harming 
  us, instead of just checking attack strength > 1.
**************************************************************************/
bool adv_data_phase_init(struct player *pplayer, bool is_new_phase)
{
  struct adv_data *ai = pplayer->server.adv;
  int i;
  int nuke_units;
  bool danger_of_nukes;

  fc_assert_ret_val(ai != NULL, FALSE);

  if (ai->phase_is_initialized) {
    return FALSE;
  }
  ai->phase_is_initialized = TRUE;

  TIMING_LOG(AIT_AIDATA, TIMER_START);

  nuke_units = num_role_units(F_NUCLEAR);
  danger_of_nukes = FALSE;

  /*** Threats ***/

  ai->num_continents    = map.num_continents;
  ai->num_oceans        = map.num_oceans;
  ai->threats.continent = fc_calloc(ai->num_continents + 1, sizeof(bool));
  ai->threats.invasions = FALSE;
  ai->threats.nuclear   = 0; /* none */
  ai->threats.ocean     = fc_calloc(ai->num_oceans + 1, sizeof(bool));
  ai->threats.igwall    = FALSE;

  players_iterate(aplayer) {
    if (!is_player_dangerous(pplayer, aplayer)) {
      continue;
    }

    /* The idea is that if there aren't any hostile cities on
     * our continent, the danger of land attacks is not big
     * enough to warrant city walls. Concentrate instead on 
     * coastal fortresses and hunting down enemy transports. */
    city_list_iterate(aplayer->cities, acity) {
      Continent_id continent = tile_continent(acity->tile);
      if (continent >= 0) {
        ai->threats.continent[continent] = TRUE;
      }
    } city_list_iterate_end;

    unit_list_iterate(aplayer->units, punit) {
      if (unit_has_type_flag(punit, F_IGWALL)) {
        ai->threats.igwall = TRUE;
      }

      if (is_sailing_unit(punit)) {
        /* If the enemy has not started sailing yet, or we have total
         * control over the seas, don't worry, keep attacking. */
        if (get_transporter_capacity(punit) > 0) {
          unit_class_iterate(punitclass) {
            if (punitclass->move_type == UMT_LAND
                && can_unit_type_transport(unit_type(punit), punitclass)) {
              /* Enemy can transport some land units! */
              ai->threats.invasions = TRUE;
              break;
            }
          } unit_class_iterate_end;
        }

        /* The idea is that while our enemies don't have any offensive
         * seaborne units, we don't have to worry. Go on the offensive! */
        if (unit_type(punit)->attack_strength > 1) {
	  if (is_ocean_tile(unit_tile(punit))) {
	    Continent_id continent = tile_continent(unit_tile(punit));
	    ai->threats.ocean[-continent] = TRUE;
	  } else {
	    adjc_iterate(unit_tile(punit), tile2) {
	      if (is_ocean_tile(tile2)) {
	        Continent_id continent = tile_continent(tile2);
	        ai->threats.ocean[-continent] = TRUE;
	      }
	    } adjc_iterate_end;
	  }
        } 
        continue;
      }

      /* If our enemy builds missiles, worry about missile defence. */
      if (uclass_has_flag(unit_class(punit), UCF_MISSILE)
          && unit_type(punit)->attack_strength > 1) {
        ai->threats.missile = TRUE;
      }

      /* If he builds nukes, worry a lot. */
      if (unit_has_type_flag(punit, F_NUCLEAR)) {
        danger_of_nukes = TRUE;
      }
    } unit_list_iterate_end;

    /* Check for nuke capability */
    for (i = 0; i < nuke_units; i++) {
      struct unit_type *nuke = get_role_unit(F_NUCLEAR, i);

      if (can_player_build_unit_direct(aplayer, nuke)) { 
        ai->threats.nuclear = 1;
      }
    }
  } players_iterate_end;

  /* Increase from fear to terror if opponent actually has nukes */
  if (danger_of_nukes) {
    ai->threats.nuclear++; /* sum of both fears */
  }

  /*** Exploration ***/

  ai->explore.land_done = TRUE;
  ai->explore.sea_done = TRUE;
  ai->explore.continent = fc_calloc(ai->num_continents + 1, sizeof(bool));
  ai->explore.ocean = fc_calloc(ai->num_oceans + 1, sizeof(bool));
  whole_map_iterate(ptile) {
    Continent_id continent = tile_continent(ptile);

    if (is_ocean_tile(ptile)) {
      if (ai->explore.sea_done && ai_handicap(pplayer, H_TARGETS) 
          && !map_is_known(ptile, pplayer)) {
	/* We're not done there. */
        ai->explore.sea_done = FALSE;
        ai->explore.ocean[-continent] = TRUE;
      }
      /* skip rest, which is land only */
      continue;
    }
    if (ai->explore.continent[tile_continent(ptile)]) {
      /* we don't need more explaining, we got the point */
      continue;
    }
    if (tile_has_special(ptile, S_HUT) 
        && (!ai_handicap(pplayer, H_HUTS)
             || map_is_known(ptile, pplayer))) {
      ai->explore.land_done = FALSE;
      ai->explore.continent[continent] = TRUE;
      continue;
    }
    if (ai_handicap(pplayer, H_TARGETS) && !map_is_known(ptile, pplayer)) {
      /* this AI must explore */
      ai->explore.land_done = FALSE;
      ai->explore.continent[continent] = TRUE;
    }
  } whole_map_iterate_end;

  /*** Statistics ***/

  ai->stats.workers = fc_calloc(ai->num_continents + 1, sizeof(int));
  ai->stats.cities = fc_calloc(ai->num_continents + 1, sizeof(int));
  ai->stats.average_production = 0;
  city_list_iterate(pplayer->cities, pcity) {
    Continent_id continent = tile_continent(pcity->tile);
    if (continent >= 0) {
      ai->stats.cities[continent]++;
    }
    ai->stats.average_production += pcity->surplus[O_SHIELD];
  } city_list_iterate_end;
  ai->stats.average_production /= MAX(1, city_list_size(pplayer->cities));
  BV_CLR_ALL(ai->stats.diplomat_reservations);
  unit_list_iterate(pplayer->units, punit) {
    struct tile *ptile = unit_tile(punit);

    if (!is_ocean_tile(ptile) && unit_has_type_flag(punit, F_SETTLERS)) {
      ai->stats.workers[(int)tile_continent(unit_tile(punit))]++;
    }
    if (unit_has_type_flag(punit, F_DIPLOMAT) && def_ai_unit_data(punit)->task == AIUNIT_ATTACK) {
      /* Heading somewhere on a mission, reserve target. */
      struct city *pcity = tile_city(punit->goto_tile);

      if (pcity) {
        BV_SET(ai->stats.diplomat_reservations, pcity->id);
      }
    }
  } unit_list_iterate_end;

  /*** Diplomacy ***/

  ai->dipl.spacerace_leader = player_leading_spacerace();

  ai->dipl.production_leader = NULL;
  players_iterate(aplayer) {
    if (ai->dipl.production_leader == NULL
        || ai->dipl.production_leader->score.mfg < aplayer->score.mfg) {
      ai->dipl.production_leader = aplayer;
    }
  } players_iterate_end;

  /*** Priorities ***/

  /* NEVER set these to zero! Weight values are usually multiplied by 
   * these values, so be careful with them. They are used in city 
   * and government calculations, and food and shields should be 
   * slightly bigger because we only look at surpluses there. They
   * are all WAGs. */
  ai->food_priority = FOOD_WEIGHTING;
  ai->shield_priority = SHIELD_WEIGHTING;
  if (adv_wants_science(pplayer)) {
    ai->luxury_priority = 1;
    ai->science_priority = TRADE_WEIGHTING;
  } else {
    ai->luxury_priority = TRADE_WEIGHTING;
    ai->science_priority = 1;
  }
  ai->gold_priority = TRADE_WEIGHTING;
  ai->happy_priority = 1;
  ai->unhappy_priority = TRADE_WEIGHTING; /* danger */
  ai->angry_priority = TRADE_WEIGHTING * 3; /* grave danger */
  ai->pollution_priority = POLLUTION_WEIGHTING;

  /*** Interception engine ***/

  /* We are tracking a unit if punit->server.ai->cur_pos is not NULL. If we
   * are not tracking, start tracking by setting cur_pos. If we are, 
   * fill prev_pos with previous cur_pos. This way we get the 
   * necessary coordinates to calculate a probably trajectory. */
  players_iterate(aplayer) {
    if (!aplayer->is_alive || aplayer == pplayer) {
      continue;
    }
    unit_list_iterate(aplayer->units, punit) {
      struct unit_ai *unit_data = def_ai_unit_data(punit);

      if (!unit_data->cur_pos) {
        /* Start tracking */
        unit_data->cur_pos = &unit_data->cur_struct;
        unit_data->prev_pos = NULL;
      } else {
        unit_data->prev_struct = unit_data->cur_struct;
        unit_data->prev_pos = &unit_data->prev_struct;
      }
      *unit_data->cur_pos = unit_tile(punit);
    } unit_list_iterate_end;
  } players_iterate_end;
  
  /* Research want */
  if (is_future_tech(player_research_get(pplayer)->researching)
      || player_has_really_useful_tech_parasite(pplayer)) {
    ai->wants_science = FALSE;
  } else {
    ai->wants_science = TRUE;
  }
  
  /* max num cities
   * The idea behind this code is that novice players don't understand that
   * expansion is critical and find it very annoying.
   * With the following code AI players will try to be only a bit better 
   * than the best human players. This should lead to more exciting games
   * for the beginners.
   */
  if (ai_handicap(pplayer, H_EXPANSION)) {
    bool found_human = FALSE;
    ai->max_num_cities = 3;
    players_iterate(aplayer) {
      if (aplayer == pplayer || aplayer->ai_controlled
          || !aplayer->is_alive) {
        continue;
      }
      ai->max_num_cities = MAX(ai->max_num_cities,
                               city_list_size(aplayer->cities) + 3);
      found_human = TRUE;
    } players_iterate_end;
    if (!found_human) {
      ai->max_num_cities = MAP_INDEX_SIZE;
    }
  } else {
    ai->max_num_cities = MAP_INDEX_SIZE;
  }

  count_my_units(pplayer);

  TIMING_LOG(AIT_AIDATA, TIMER_STOP);

  /* Government */
  TIMING_LOG(AIT_GOVERNMENT, TIMER_START);
  adv_best_government(pplayer);
  TIMING_LOG(AIT_GOVERNMENT, TIMER_STOP);

  return TRUE;
}

/**************************************************************************
  Clean up our mess.
**************************************************************************/
void adv_data_phase_done(struct player *pplayer)
{
  struct adv_data *ai = pplayer->server.adv;

  fc_assert_ret(ai != NULL);

  if (!ai->phase_is_initialized) {
    return;
  }

  free(ai->explore.ocean);
  ai->explore.ocean = NULL;

  free(ai->explore.continent);
  ai->explore.continent = NULL;

  free(ai->threats.continent);
  ai->threats.continent = NULL;

  free(ai->threats.ocean);
  ai->threats.ocean = NULL;

  free(ai->stats.workers);
  ai->stats.workers = NULL;

  free(ai->stats.cities);
  ai->stats.cities = NULL;

  ai->num_continents = 0;
  ai->num_oceans     = 0;

  ai->phase_is_initialized = FALSE;
}

/**************************************************************************
  Return a pointer to our data
**************************************************************************/
struct adv_data *adv_data_get(struct player *pplayer)
{
  struct adv_data *adv = pplayer->server.adv;

  fc_assert_ret_val(adv != NULL, NULL);

  /* It's certainly indication of bug causing problems
     if this adv_data_get() gets called between adv_data_phase_done() and
     adv_data_phase_init(), since we may end up calling those
     functions if number of known continents has changed.

     Consider following case:
       Correct call order would be:
       a) adv_data_phase_init()
       b)   adv_data_get() -> adv_data_phase_done()
       c)   adv_data_get() -> adv_data_phase_init()
       d) adv_data_phase_done()
       e) do something
       f) adv_data_phase_init()

       In (e) data phase would be closed and data would be
       correctly initialized at (f), which is probably beginning
       next turn.

       Buggy version where adv_data_get() (b&c) gets called after (d):
       a) adv_data_phase_init()
       d) adv_data_phase_done()
       b)   adv_data_get() -> adv_data_phase_done()
       c)   adv_data_get() -> adv_data_phase_init()
       e) do something
       f) adv_data_phase_init()

       Now in (e) data phase would be open. When adv_data_phase_init()
       then finally gets called and it really should recreate data
       to match situation of new turn, it detects that data phase
       is already initialized and does nothing.

       So, this assertion is here for a reason! */
  fc_assert(adv->phase_is_initialized);

  if (adv->num_continents != map.num_continents
      || adv->num_oceans != map.num_oceans) {
    /* we discovered more continents, recalculate! */
    adv_data_phase_done(pplayer);
    adv_data_phase_init(pplayer, FALSE);
  }

  return adv;
}

/**************************************************************************
  Allocate memory for advisor data. Save to call multiple times.
**************************************************************************/
void adv_data_init(struct player *pplayer)
{
  struct adv_data *ai;

  if (pplayer->server.adv == NULL) {
    pplayer->server.adv = fc_calloc(1, sizeof(*pplayer->server.adv));
  }
  ai = pplayer->server.adv;

  ai->government_want = fc_calloc(government_count() + 1,
                                  sizeof(*ai->government_want));

  ai_data_default(pplayer);
}

/**************************************************************************
  Initialize with sane values.
**************************************************************************/
void ai_data_default(struct player *pplayer)
{
  struct adv_data *ai = pplayer->server.adv;

  fc_assert_ret(ai != NULL);

  ai->govt_reeval = 0;
  memset(ai->government_want, 0,
         (government_count() + 1) * sizeof(*ai->government_want));

  ai->wonder_city = 0;

  ai->wants_science = TRUE;
  ai->celebrate = FALSE;
  ai->max_num_cities = 10000;
}

/**************************************************************************
  Free memory for advisor data.
**************************************************************************/
void adv_data_close(struct player *pplayer)
{
  struct adv_data *ai = pplayer->server.adv;

  fc_assert_ret(NULL != ai);

  adv_data_phase_done(pplayer);

  if (ai->government_want != NULL) {
    free(ai->government_want);
  }

  if (ai != NULL) {
    free(ai);
  }
  pplayer->ai = NULL;
}

/**************************************************************************
  Find best government to aim for.
  We do it by setting our government to all possible values and calculating
  our GDP (total ai_eval_calc_city) under this government.  If the very
  best of the governments is not available to us (it is not yet discovered),
  we record it in the goal.gov structure with the aim of wanting the
  necessary tech more.  The best of the available governments is recorded 
  in goal.revolution.  We record the want of each government, and only
  recalculate this data every ai->govt_reeval_turns turns.

  Note: Call this _before_ doing taxes!
**************************************************************************/
void adv_best_government(struct player *pplayer)
{
  struct adv_data *adv = adv_data_get(pplayer);
  int best_val = 0;
  int bonus = 0; /* in percentage */
  struct government *current_gov = government_of_player(pplayer);

  adv->goal.govt.gov = current_gov;
  adv->goal.govt.val = 0;
  adv->goal.govt.req = A_UNSET;
  adv->goal.revolution = current_gov;

  if (ai_handicap(pplayer, H_AWAY) || !pplayer->is_alive) {
    return;
  }

  if (adv->govt_reeval == 0) {
    governments_iterate(gov) {
      int val = 0;
      int dist;

      if (gov == game.government_during_revolution) {
        continue; /* pointless */
      }
      if (gov->ai.better
          && can_change_to_government(pplayer, gov->ai.better)) {
        continue; /* we have better governments available */
      }
      pplayer->government = gov;
      /* Ideally we should change tax rates here, but since
       * this is a rather big CPU operation, we'd rather not. */
      check_player_max_rates(pplayer);
      city_list_iterate(pplayer->cities, acity) {
        auto_arrange_workers(acity);
      } city_list_iterate_end;
      city_list_iterate(pplayer->cities, pcity) {
        val += ai_eval_calc_city(pcity, adv);
      } city_list_iterate_end;

      /* Bonuses for non-economic abilities. We increase val by
       * a very small amount here to choose govt in cases where
       * we have no cities yet. */
      bonus += get_player_bonus(pplayer, EFT_VETERAN_BUILD) ? 3 : 0;
      bonus -= get_player_bonus(pplayer, EFT_REVOLUTION_WHEN_UNHAPPY) ? 3 : 0;
      bonus += get_player_bonus(pplayer, EFT_NO_INCITE) ? 4 : 0;
      bonus += get_player_bonus(pplayer, EFT_UNBRIBABLE_UNITS) ? 2 : 0;
      bonus += get_player_bonus(pplayer, EFT_INSPIRE_PARTISANS) ? 3 : 0;
      bonus += get_player_bonus(pplayer, EFT_RAPTURE_GROW) ? 2 : 0;
      bonus += get_player_bonus(pplayer, EFT_FANATICS) ? 3 : 0;
      bonus += get_player_bonus(pplayer, EFT_OUTPUT_INC_TILE) * 8;

      val += (val * bonus) / 100;

      /* FIXME: handle reqs other than technologies. */
      dist = 0;
      requirement_vector_iterate(&gov->reqs, preq) {
	if (VUT_ADVANCE == preq->source.kind) {
	  dist += MAX(1, num_unknown_techs_for_goal(pplayer,
						    advance_number(preq->source.value.advance)));
	}
      } requirement_vector_iterate_end;
      val = amortize(val, dist);
      adv->government_want[government_index(gov)] = val; /* Save want */
    } governments_iterate_end;
    /* Now reset our gov to it's real state. */
    pplayer->government = current_gov;
    city_list_iterate(pplayer->cities, acity) {
      auto_arrange_workers(acity);
    } city_list_iterate_end;
    adv->govt_reeval = CLIP(5, city_list_size(pplayer->cities), 20);
  }
  adv->govt_reeval--;

  /* Figure out which government is the best for us this turn. */
  governments_iterate(gov) {
    int gi = government_index(gov);
    if (adv->government_want[gi] > best_val 
        && can_change_to_government(pplayer, gov)) {
      best_val = adv->government_want[gi];
      adv->goal.revolution = gov;
    }
    if (adv->government_want[gi] > adv->goal.govt.val) {
      adv->goal.govt.gov = gov;
      adv->goal.govt.val = adv->government_want[gi];

      /* FIXME: handle reqs other than technologies. */
      adv->goal.govt.req = A_NONE;
      requirement_vector_iterate(&gov->reqs, preq) {
	if (VUT_ADVANCE == preq->source.kind) {
	  adv->goal.govt.req = advance_number(preq->source.value.advance);
	  break;
	}
      } requirement_vector_iterate_end;
    }
  } governments_iterate_end;
  /* Goodness of the ideal gov is calculated relative to the goodness of the
   * best of the available ones. */
  adv->goal.govt.val -= best_val;
}

/**************************************************************************
  Return whether science would help us at all.
**************************************************************************/
bool adv_wants_science(struct player *pplayer)
{
  return adv_data_get(pplayer)->wants_science;
}
