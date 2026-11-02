#include "session/player_session_internal.h"

namespace aoc {
namespace worldd {

using namespace ::sandal::worldd::session::detail;

void PlayerSession::tally_speed(const GoodPosition& before, const GoodPosition& after, bool sprinting, bool evading) {
    if (!before.has_time_stamp || evading) return;
    const double seconds = static_cast<double>(after.time_stamp) - before.time_stamp;
    const double cm = std::hypot(after.x - before.x, after.y - before.y);
    if (!(seconds > 0 && seconds < 0.5) || cm < 1.0) return;
    SpeedTally& t = sprinting ? sprint_speed_ : run_speed_;
    t.cm += cm;
    t.seconds += seconds;
    if (sprinting) {
        sprint_seen_ = true;
        return;
    }
    if (!sprint_seen_ || sprint_speed_.seconds <= 0) return;
    const auto average = [](const SpeedTally& s) { return s.seconds > 0 ? s.cm / s.seconds : 0.0; };
    spdlog::log(level_for(speed_logged_),
                "[session] {}: ground speed while sprinting {:.0f} cm/s over {:.1f} s; while running (not sprinting) {:.0f} cm/s "
                "over {:.1f} s (dodges left out; 600 x the sprint's MoveSpeedMult while sprinting: 720 with no talents, 780 for a level-25 Bard)",
                name(), average(sprint_speed_), sprint_speed_.seconds, average(run_speed_), run_speed_.seconds);
    sprint_speed_ = {};
    run_speed_ = {};
    sprint_seen_ = false;
}

void PlayerSession::log_position(Clock::time_point) {
    const GoodPosition at = checker_.good();
    std::string ground;
    if (npc_world_ && npc_world_->nav()) {
        const nav::AreaNav* nav = npc_world_->nav();
        const nav::ChunkKey k = nav->chunk_at(at.x, at.y);
        ground = "; ground chunk (" + std::to_string(k.x) + ", " + std::to_string(k.y) + ") " + (nav->ready_at(at.x, at.y) ? "ready" : "NOT LOADED YET") +
                 (nav->has_region() && !nav->in_region(at.x, at.y) ? ", outside the Riverlands" : "");
    }
    const double from_login = npc_world_ ? std::hypot(at.x - npc_world_->area().center_x, at.y - npc_world_->area().center_y) / 100.0 : 0.0;
    spdlog::info("[session] {}: at ({:.0f}, {:.0f}, {:.0f}) cm, {:.0f} m from the login spot, movement mode {}{}; {} moves acknowledged, {} refused, {} dropped as stale{}; its view holds {} awake and {} dormant actors",
                 name(), at.x, at.y, at.z, from_login, static_cast<unsigned>(at.movement_mode), mounted() ? " (mounted)" : "", moves_acked_, moves_refused_, moves_stale_, ground,
                 view_ ? view_->count(world::ClientState::Awake) : 0, view_ ? view_->count(world::ClientState::Dormant) : 0);
}

const schema::ClassSchema* PlayerSession::class_of(Role role) const {
    switch (role) {
        case Role::PlayerController: return pb_.player_controller().obj.schema;
        case Role::GameState: return pb_.game_state().obj.schema;
        case Role::Pawn: return pb_.pawn().obj.schema;
        case Role::PlayerState: return pb_.player_state().obj.schema;
    }
    return nullptr;
}

bool PlayerSession::role_of_channel(uint32_t channel, Role* role) const {
    for (size_t i = 0; i < channels_.size(); ++i) {
        if (channels_[i] == channel) {
            *role = static_cast<Role>(i);
            return true;
        }
    }
    return false;
}

void PlayerSession::on_bunch(Connection& conn, const InBunch& bunch, Clock::time_point now) {
    if (mounted() && bunch.channel != 0 && bunch.channel == mount_channel()) return on_mount_bunch(conn, bunch, now);
    Role role;
    if (!role_of_channel(bunch.channel, &role)) {
        spdlog::debug("[session] {}: ignoring a bunch on channel {}, which is none of the player's", name(), bunch.channel);
        return;
    }
    if (bunch.close || bunch.bits == 0) return;
    const schema::ClassSchema* cls = class_of(role);
    const repl::Actor* actor = role == Role::PlayerController ? &pb_.player_controller()
                               : role == Role::GameState      ? &pb_.game_state()
                               : role == Role::Pawn           ? &pb_.pawn()
                                                              : &pb_.player_state();
    const auto resolve = [actor](const FIntrepidNetworkGUID& g) -> const schema::ClassSchema* {
        const repl::Subobject* sub = actor->find(g);
        return sub ? sub->obj.schema : nullptr;
    };
    repl::CodecOptions client_opts;
    client_opts.inline_objects = (role == Role::Pawn);
    const auto parsed = repl::read_client_bunch(*cls, resolve, bunch.data.data(), bunch.bits, client_opts);
    if (!parsed) {
        static const char* const kHex = "0123456789abcdef";
        std::string head;
        for (size_t i = 0; i < bunch.data.size() && i < 160; ++i) {
            head += kHex[bunch.data[i] >> 4];
            head += kHex[bunch.data[i] & 15];
        }
        ++malformed_bunches_;
        const std::string kind = std::to_string(bunch.bits) + ":" + head.substr(0, 16);
        const bool new_kind = unread_kinds_.size() < kMaxUnreadKinds && unread_kinds_.insert(kind).second;
        if (malformed_bunches_ <= 3 || new_kind) {
            spdlog::warn("[session] {}: a bunch of {} bits on channel {} ({}) does not read: ignored (starts {})", name(), bunch.bits, bunch.channel, role_name(role), head);
        } else {
            spdlog::log(malformed_bunches_ % 10 == 0 ? spdlog::level::info : spdlog::level::debug,
                        "[session] {}: a bunch of {} bits on channel {} ({}) does not read: ignored ({} so far; starts {})", name(), bunch.bits, bunch.channel, role_name(role), malformed_bunches_,
                        head.substr(0, 48));
        }
        return;
    }
    note_unserved_blocks(role, *parsed, bunch);
    for (const repl::ClientRpc& rpc : parsed->rpcs) {
        ++client_calls_[rpc.call.rpc->name];   // (by the schema's own string: no allocation; the summary at the session's end adds the classes' copies of a name up)
        dispatch(conn, role, rpc, bunch, now);
    }
}

void PlayerSession::note_unserved_blocks(Role role, const repl::ClientBunch& parsed, const InBunch& bunch) {
    if (parsed.in_place_blocks > 0) {
        in_place_blocks_ += parsed.in_place_blocks;
        ++in_place_bunches_;
        static const char* const kHex = "0123456789abcdef";
        for (const repl::InPlaceBlock& block : parsed.in_place) {
            const repl::InPlaceName& n = block.name;
            const std::string key = std::to_string(n.outer.ObjectId) + "/" + n.path;
            const auto seen = in_place_names_.find(key);
            if (seen != in_place_names_.end()) {
                ++seen->second;
                continue;
            }
            if (in_place_names_.size() >= kMaxInPlaceNames) continue;   // (the names beyond these are only counted)
            in_place_names_[key] = 1;
            std::string whole;
            for (size_t i = 0; i < bunch.data.size() && i < 160; ++i) {
                whole += kHex[bunch.data[i] >> 4];
                whole += kHex[bunch.data[i] & 15];
            }
            spdlog::warn("[session] {}: the client calls an RPC on a subobject of its {} that it names by path (\"{}\", outer {}/{}/{}, export flags {}, {} payload bits): worldd "
                         "replicates no such subobject, so the call is skipped and the rest of its bunch is read; said once per name; the bunch is {}, {} bits, and starts {}",
                         name(), role_name(role), n.path, n.outer.ObjectId, n.outer.ServerId, n.outer.Randomizer, static_cast<unsigned>(n.flags), block.payload_bits,
                         bunch.reliable ? "reliable" : "unreliable", bunch.bits, whole);
        }
        if (!parsed.rpcs.empty() && ++in_place_shared_logged_ <= kMaxInPlaceSharedLogged) {
            std::string others;
            for (const repl::ClientRpc& rpc : parsed.rpcs) others += (others.empty() ? "" : ", ") + std::string(rpc.call.rpc ? rpc.call.rpc->name : "?");
            spdlog::info("[session] {}: a {} bunch of {} bits held {} calls named by path and {} others ({}): the others are read and answered as usual", name(),
                         bunch.reliable ? "reliable" : "unreliable", bunch.bits, parsed.in_place_blocks, parsed.rpcs.size(), others);
        }
        for (size_t cutoff = 10; cutoff <= in_place_blocks_; cutoff *= 10) {
            if (in_place_blocks_ - parsed.in_place_blocks < cutoff && in_place_blocks_ >= cutoff) {
                spdlog::info("[session] {}: {} calls on subobjects the client names by path so far, skipped ({} bunches, {} names)", name(), in_place_blocks_, in_place_bunches_,
                             in_place_names_.size());
            }
        }
    }
    if (parsed.skipped_fields.count > 0) {
        skipped_fields_ += parsed.skipped_fields.count;
        if (++skipped_fields_logged_ <= kLoggedPerKind) {
            std::string ids;
            for (const uint32_t id : parsed.skipped_fields.first) ids += (ids.empty() ? "" : ", ") + std::to_string(id);
            spdlog::warn("[session] {}: the client sent {} RPC fields on the {}'s channel that its class has no RPC for (field indices {}): skipped, the rest of the bunch is read", name(),
                         parsed.skipped_fields.count, role_name(role), ids);
        }
    }
}

bool PlayerSessionTypes::SessionInternal::dispatch_controller_early_rpc(PlayerSession& s, Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    const std::string_view name_of_rpc = rpc.call.rpc->name;
    if (name_of_rpc == "ServerAcknowledgePossession") {
        const auto it = rpc.call.params.values.find(1);
        const auto* pawn = it != rpc.call.params.values.end() ? it->second.get_if<FIntrepidNetworkGUID>() : nullptr;
        const bool its_pawn = pawn && *pawn == s.pb_.setup().guids.pawn;
        const bool its_mount = pawn && s.ride_ && s.mounted() && *pawn == s.mount_guid_;
        if (s.possession_logged_ >= kLoggedPerKind) {
            ++s.possession_logged_;
            spdlog::debug("[session] {}: possession ack #{} from the client ({})", s.name(), s.possession_logged_,
                          its_pawn ? "its pawn" : its_mount ? "its mount" : "not its pawn");
        } else if (its_pawn || its_mount) {
            ++s.possession_logged_;
            spdlog::info("[session] {}: the client acknowledged possession of its {}", s.name(), its_mount ? "mount" : "pawn");
        } else {
            ++s.possession_logged_;
            spdlog::warn("[session] {}: the client acknowledged possession of {}, not of its pawn", s.name(),
                         pawn ? "another object" : "nothing readable");
        }
        return true;
    }
    if (name_of_rpc == "ServerStartInteractionWithFoliage") {
        if (!rpc.call.decoded && ++s.foliage_unreadable_logged_ <= kLoggedPerKind) {
            spdlog::warn("[session] {}: ServerStartInteractionWithFoliage arrived and its parameters do not decode: no cutting starts, and the client is told to end its own (ClientInterruptInteractionWithFoliage)", s.name());
        }
        s.on_foliage_interact(conn, rpc, now);
        return true;
    }
    return false;
}

bool PlayerSessionTypes::SessionInternal::dispatch_controller_actor_rpc(PlayerSession& s, Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (dispatch_controller_early_rpc(s, conn, rpc, now)) return true;
    const std::string_view name_of_rpc = rpc.call.rpc->name;
    if (name_of_rpc == "ServerDesummonMount") { s.on_desummon_mount(conn, now); return true; }
    if (name_of_rpc == "ServerDismountInteraction") { s.on_dismount_request(conn, now); return true; }
    if (name_of_rpc == "ServerRequestClientLoad") { s.on_client_load(conn, now); return true; }
    if (name_of_rpc == "ServerUpdateAccountCombatSettings") { s.on_combat_settings(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerRequestStorageEvent") { s.on_storage_event_request(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerRequestWarehouseSummary" || name_of_rpc == "ServerVisitWarehouse") { s.on_warehouse_request(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerCloseWarehouse") { s.on_warehouse_close(conn); return true; }
    if (name_of_rpc == "ServerSetActionbarSlotUseData") { s.on_set_slot(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerSelectDialogueChoice") { s.on_dialogue_choice(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerAbandonQuests") { s.on_abandon_quests(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerRequestItemDelivery") { s.on_item_delivery(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerSetQuestBlacklisted") {
        if (s.quest_told_.insert("ServerSetQuestBlacklisted").second) spdlog::info("[session] {}: ServerSetQuestBlacklisted is accepted and ignored (there is no Destiny track yet; told once)", s.name());
        return true;
    }
    if (name_of_rpc == "ServerDebugStartQuest" || name_of_rpc == "ServerDebugEndQuest" || name_of_rpc == "ServerDebugFailQuestByInstance" || name_of_rpc == "ServerClearCompletedQuests") {
        s.on_quest_debug_rpc(conn, rpc, now);
        return true;
    }
    if (name_of_rpc == "ServerHandleVendorPurchase") { s.on_vendor_purchase(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerStartCraft") { s.on_start_craft(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerSendProcessingStationRequest") { s.on_processing_request(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerInterruptStationCraft") { s.on_interrupt_station_craft(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerOpenVendor") { s.on_open_vendor(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerRequestLootStorageEvents") { s.on_loot_request(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerGetNodeTaxTable") { s.on_node_tax_request(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerRequestNodeSummary") { s.on_node_summary_request(rpc); return true; }
    if (name_of_rpc == "ServerRequestCurrentSiegeSummary") { s.on_siege_summary_request(rpc); return true; }
    if (name_of_rpc == "ServerRequestAllNodeSiegeCooldowns") { s.on_siege_cooldowns_request(); return true; }
    if (name_of_rpc == "ServerSetEssentialCitizenshipWindowData") { s.on_citizenship_window_request(); return true; }
    if (name_of_rpc == "ServerSetNodeLevel" || name_of_rpc == "ServerAddNodeExperience" || name_of_rpc.substr(0, 8) == "ServerGM") { s.on_gm_node_rpc(rpc, now); return true; }
    if (name_of_rpc == "ServerTeleportXYZ") { s.on_gm_teleport_xyz(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerTeleport") { s.on_gm_teleport_key(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerTeleportPlayer") { s.on_gm_teleport_player(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerHandleVendorTransaction") { s.on_vendor_transaction(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerSetPvpFlag") { s.on_set_pvp_flag(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerCloseLoot") { s.on_close_loot(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerSetActionbarMultiSlotUseData") { s.on_set_slots(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerSetMapMarkerLocation") { s.on_set_map_marker(conn, rpc, now); return true; }
    return false;
}

bool PlayerSessionTypes::SessionInternal::dispatch_pawn_actor_rpc(PlayerSession& s, Connection& conn, const repl::ClientRpc& rpc, const InBunch& bunch, Clock::time_point now) {
    const std::string_view name_of_rpc = rpc.call.rpc->name;
    if (name_of_rpc == "ServerClientIsReadyToPhaseIn") {
        if (s.phase_in_requested_ || !s.room_for(1)) return true;
        s.phase_in_requested_ = true;
        spdlog::info("[session] {}: the client is ready to phase in", s.name());
        if (s.scenery_opened_ > 0) {
            spdlog::info("[join] {}: phase-in after {:.1f} s (join to the client's ready call; {} town replicator{} sent before it, the last at {:.1f} s)", s.name(), s.session_seconds(now),
                         s.scenery_opened_, s.scenery_opened_ == 1 ? "" : "s", s.scenery_at_);
        } else {
            spdlog::info("[join] {}: phase-in after {:.1f} s (join to the client's ready call; no town replicator was sent before it)", s.name(), s.session_seconds(now));
        }
        s.queue("phase-in", [&s](bootstrap::PlayerBootstrap& pb) {
            auto send = pb.phase_in();
            if (send) s.phase_in_done_ = true;
            return send;
        }, conn, /*must_build=*/true);
        s.time_syncs_ = {now + kTimeSyncAfterPhaseIn, now + kTimeSyncSecondAfterPhaseIn};
        return true;
    }
    if (name_of_rpc == "ServerRequestStorageData") { s.on_storage_request(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerFetchWarehouse") { s.on_warehouse_request(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerMovePacked") { s.on_move(conn, rpc, bunch, now, /*on_mount=*/false); return true; }
    if (name_of_rpc == "ServerItemUsed") { s.on_item_used(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerPurchaseSkillBuild") { s.on_purchase(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerInteractWithInteractableComponent") { s.on_interact(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerInterruptChannel") { s.on_interrupt_channel(conn, now); return true; }
    if (name_of_rpc == "RequestServerRespawnChoices") { s.on_respawn_choices_request(conn, rpc, now); return true; }
    if (name_of_rpc == "RequestServerSetHomeAttunement") { s.on_home_attunement(conn, rpc, now); return true; }
    if (name_of_rpc == "Respawn") { s.on_plain_respawn(conn, now); return true; }
    if (name_of_rpc == "SendServerRespawnChoice") { s.on_respawn_choice(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerSetUseRangedWeapon") { s.on_set_ranged(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerSetActiveTarget") { s.on_set_active_target(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerSetHoverTargetGuid") { s.on_set_hover_target(conn, rpc, now); return true; }
    if (name_of_rpc == "ServerActivateItem") {
        if (!s.activate_logged_) {
            s.activate_logged_ = true;
            spdlog::debug("[session] {}: ServerActivateItem is not answered: ServerItemUsed carries the same request", s.name());
        }
        return true;
    }
    return false;
}

void PlayerSession::dispatch(Connection& conn, Role role, const repl::ClientRpc& rpc, const InBunch& bunch, Clock::time_point now) {
    const std::string_view name_of_rpc = rpc.call.rpc->name;
    if (rpc.on_actor && role == Role::PlayerController) {
        if (SessionInternal::dispatch_controller_actor_rpc(*this, conn, rpc, now)) return;
    } else if (rpc.on_actor && role == Role::Pawn) {
        if (SessionInternal::dispatch_pawn_actor_rpc(*this, conn, rpc, bunch, now)) return;
    } else if (!rpc.on_actor && role == Role::Pawn && rpc.object == pb_.setup().guids.ability) {
        if (name_of_rpc == "ServerTriggerAbility") return on_ability_trigger(conn, rpc, bunch, now);
        if (name_of_rpc == "ServerNotifyHeldAbilityCancelled") return on_ability_cancel(conn, rpc, now);
        if (name_of_rpc == "ServerRequestBranchVerification") return on_branch_request(conn, rpc, now);
        if (name_of_rpc == "RequestHitVerification_Server") return on_hit_request(conn, rpc, now);
        if (name_of_rpc == "ServerEndAbility") return on_ability_end(conn, rpc, now);
    } else if (!rpc.on_actor && role == Role::PlayerController && rpc.object == pb_.setup().guids.ftue) {
        if (name_of_rpc == "Server_RemoveLocaleFTUEs") return on_remove_ftues(conn, rpc, now);
    }
    if (unhandled_logged_.insert(std::string(name_of_rpc)).second) {
        spdlog::info("[session] {}: ignoring the client's {} ({}): worldd does not answer it (once per call)", name(), name_of_rpc, role_name(role));
    }
}

bool PlayerSessionTypes::SessionInternal::check_stale_or_borderline_move(PlayerSession& s, const repl::NetworkMove& move, const MoveVerdict& verdict) {
    if (verdict.fault == MoveFault::TimeStampNotRising) {
        if (++s.stale_streak_ >= kStaleStreakLog && !s.stale_streak_logged_) {
            s.stale_streak_logged_ = true;
            spdlog::info("[session] {}: {} moves in a row at a TimeStamp that does not rise (this one {}, the last good {}): dropped without a reply; the client's movement clock may have restarted without the server forgetting its own",
                         s.name(), s.stale_streak_, move.time_stamp, s.checker_.good().time_stamp);
        }
        if (++s.moves_stale_ <= kLoggedPerKind) {
            spdlog::debug("[session] {}: the move at TimeStamp {} does not rise above {}: dropped without a reply", s.name(), move.time_stamp,
                          s.checker_.good().time_stamp);
        }
        return true;
    }
    s.stale_streak_ = 0;
    s.stale_streak_logged_ = false;
    if (verdict.accepted && verdict.borderline) {
        ++s.moves_borderline_;
        if (s.moves_borderline_ <= kLoggedPerKind || s.moves_borderline_ % 100 == 0) {
            spdlog::info("[session] {}: the {}move at TimeStamp {} (movement mode {}) would be refused by the flight law ({:.0f} cm in {:.3f} s, the law allows {:.0f}; speed multiplier {:.2f}, view pitch {:.1f} degrees): accepted, not corrected ({} so far)",
                         s.name(), s.mounted() ? "mount's " : "", move.time_stamp, static_cast<unsigned>(move.movement_mode), verdict.step, verdict.dt, verdict.soft_allowed,
                         s.checker_.speed_mult(),
                         (move.control_rotation.pitch >= 32768 ? move.control_rotation.pitch - 65536.0 : move.control_rotation.pitch) * 360.0 / 65536.0, s.moves_borderline_);
        }
    }
    return false;
}

void PlayerSessionTypes::SessionInternal::accept_move(PlayerSession& s, Connection& conn, const repl::NetworkMove& move, const GoodPosition& before, Clock::time_point now) {
    const bool on_mount = s.mounted();
    s.pending_fix_.reset();
    if (!on_mount && s.is_moving(before, move)) s.moved_at_ = now;
    if (AbilityRuntime* abilities = s.ability_runtime(now)) {
        const bool moving = s.is_moving(before, move);
        abilities->set_moving(moving, s.ability_seconds(now));
        if (s.pipeline_) s.pipeline_->set_moving(moving);
        if (!on_mount) s.tally_speed(before, s.checker_.good(), abilities->sprinting(), abilities->evading());
    }
    auto ack = on_mount ? s.mount_ack(move.time_stamp) : s.pb_.good_move_ack(move.time_stamp);
    if (!ack) return;
    if (s.send(conn, *ack) != Outcome::Sent) return;
    if (on_mount) ++s.mount_moves_acked_;
    if (++s.moves_acked_ == 1) spdlog::info("[session] {}: the first move is acknowledged (TimeStamp {})", s.name(), move.time_stamp);
}

void PlayerSessionTypes::SessionInternal::refuse_move(PlayerSession& s, const repl::NetworkMove& move, const MoveVerdict& verdict) {
    ++s.moves_refused_;
    if (s.moves_refused_ == 10 || s.moves_refused_ == 100 || s.moves_refused_ == 1000 || s.moves_refused_ == 10000) {
        spdlog::info("[session] {}: {} moves refused so far", s.name(), s.moves_refused_);
    }
    const GoodPosition& good = s.checker_.good();
    const repl::Vec3d at = move.location_cm();
    const spdlog::level::level_enum level = s.level_for(s.refused_logged_);
    if (verdict.step > 0) {
        spdlog::log(level, "[session] {}: the {}move at TimeStamp {} (movement mode {}) is refused ({}: {:.0f} cm in {:.3f} s, at most {:.0f}): put back at ({:.1f}, {:.1f}, {:.1f}) cm",
                    s.name(), s.mounted() ? "mount's " : "", move.time_stamp, static_cast<unsigned>(move.movement_mode), fault_name(verdict.fault), verdict.step, verdict.dt,
                    verdict.allowed, good.x, good.y, good.z);
    } else {
        spdlog::log(level, "[session] {}: the {}move at TimeStamp {} (movement mode {}) to ({:.1f}, {:.1f}, {:.1f}) cm is refused ({}): put back at ({:.1f}, {:.1f}, {:.1f}) cm",
                    s.name(), s.mounted() ? "mount's " : "", move.time_stamp, static_cast<unsigned>(move.movement_mode), at.x, at.y, at.z, fault_name(verdict.fault), good.x, good.y,
                    good.z);
    }
    s.pending_fix_ = std::isfinite(move.time_stamp) ? move.time_stamp : 0.0f;
}

void PlayerSession::on_move(Connection& conn, const repl::ClientRpc& rpc, const InBunch& bunch, Clock::time_point now, bool on_mount) {
    if (on_mount != mounted()) {
        ++moves_wrong_channel_;
        if (++ride_wrong_logged_ <= kLoggedPerKind) {
            spdlog::debug("[session] {}: a move on the {} channel while the player {}: dropped", name(), on_mount ? "mount's" : "pawn's", mounted() ? "rides" : "is on foot");
        }
        return;
    }
    std::string why;
    const auto moves = repl::read_move_packed(bunch.data.data(), bunch.data.size(), rpc.call.param_pos,
                                              rpc.call.param_pos + rpc.call.param_bits, &why);
    if (!moves) {
        if (!unreadable_move_logged_) {
            unreadable_move_logged_ = true;
            spdlog::warn("[session] {}: a ServerMovePacked does not read ({}; {} parameter bits): not acknowledged (further ones are not logged)",
                         name(), why, rpc.call.param_bits);
        }
        return;
    }
    if (!on_mount) scale_pawn_limits(now);
    const double allow = on_mount ? 0.0
                                  : std::max(dash_allowed(now) ? checker_.limits().dash_speed : 0.0,
                                             pipeline_ ? pipeline_->movement_allowance(ability_seconds(now)) : 0.0);
    const auto judge = [&](const repl::NetworkMove& m) {
        if (on_mount) {
            checker_.set_max_speed(ride_->speed_limit(m.movement_mode, ability_seconds(now)));
            checker_.set_speed_mult(ride_->effective_mult(ability_seconds(now)));
        }
        return checker_.check_allowing(m, now, allow);
    };
    for (const repl::NetworkMove* older : {moves->old ? &*moves->old : nullptr, moves->pending ? &*moves->pending : nullptr}) {
        if (older && !checker_.is_stale(older->time_stamp)) judge(*older);
    }
    const repl::NetworkMove& move = moves->new_move;
    const GoodPosition before = checker_.good();
    const MoveVerdict verdict = judge(move);
    if (on_mount && ride_) {
        const GoodPosition& g = checker_.good();
        if (g.has_time_stamp) ride_->moved(g.x, g.y, g.z, g.yaw_degrees, ability_seconds(now), g.pitch16, g.movement_mode);
    }
    if (SessionInternal::check_stale_or_borderline_move(*this, move, verdict)) return;
    if (verdict.accepted) return SessionInternal::accept_move(*this, conn, move, before, now);
    SessionInternal::refuse_move(*this, move, verdict);
}

void PlayerSession::end_of_datagram(Connection& conn) {
    if (!pending_fix_) return;
    const float time_stamp = *pending_fix_;
    pending_fix_.reset();
    if (failed_) return;
    const GoodPosition& good = checker_.good();
    const uint8_t mode = good.movement_mode == 0 ? repl::kMoveWalking : good.movement_mode;
    auto fix = mounted() ? mount_correction(time_stamp, good) : pb_.correction(time_stamp, repl::Vec3d{good.x, good.y, good.z}, repl::Vec3d{}, mode);
    if (!fix) {
        spdlog::warn("[session] {}: the correction does not build ({})", name(), pb_.error().empty() ? "no reason" : pb_.error());
        return;
    }
    if (send(conn, *fix) == Outcome::Sent) ++corrections_;
}

bool PlayerSession::is_moving(const GoodPosition& before, const repl::NetworkMove& move) const {
    if (move.input_x() != 0 || move.input_y() != 0) return true;   // the player holds a direction
    if (!before.has_time_stamp) return false;
    const double dt = static_cast<double>(move.time_stamp) - static_cast<double>(before.time_stamp);
    if (!(dt > 0)) return false;
    const repl::Vec3d at = move.location_cm();
    return std::hypot(at.x - before.x, at.y - before.y) / dt > kMovingSpeed;   // or it is going somewhere
}

}  // namespace worldd
}  // namespace aoc
