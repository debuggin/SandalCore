#include "bootstrap/character_setup.h"
#include "content/content_db.h"
#include "session/player_session.h"

namespace aoc {
namespace worldd {

namespace {

constexpr double kMountGroundUpCm = 160.0;
constexpr double kMountGroundDownCm = 1000.0;

const char* mode_name(bootstrap::GameStateMode m) {
    switch (m) {
        case bootstrap::GameStateMode::Tier1: return "tier1";
        case bootstrap::GameStateMode::Tier2: return "tier2";
        case bootstrap::GameStateMode::Tier3: return "tier3";
        case bootstrap::GameStateMode::Captured: return "captured";
    }
    return "?";
}

size_t parts_of(const Chain& c) {
    const size_t bits = c.payload.bit_pos();
    const size_t content =
        bits > Connection::kMaxSingleBunchBits ? (bits + Connection::kMaxPartialBunchBits - 1) / Connection::kMaxPartialBunchBits : 1;
    return c.exports.size() + content;
}

}  // namespace

struct PlayerSession::SessionInternal {
    static uint64_t enrich_join_setup(PlayerWorld& world, const svc::Character& character, bootstrap::CharacterSetupResult& built);
    static void init_joined_state(PlayerSession& p, PlayerWorld& world, const svc::Character& character, bootstrap::CharacterSetupResult& built, uint64_t nodes_version);
    static void init_mounts_and_view(PlayerSession& p, PlayerWorld& world);
    static void log_join_summary(PlayerSession& p, PlayerWorld& world);
};

PlayerSession::PlayerSession(bootstrap::PlayerSetup setup, bootstrap::GameStateMode mode, std::string character_id, const MovementConfig& movement,
                             PositionSink* sink, const content::AbilityRules* rules, items::Inventory inventory, const items::ItemCatalog* catalog,
                             std::optional<stats::CharacterStats> character_stats, const PlayerWorld* world, int class_id, leveling::Progress progress)
    : pb_(std::move(setup), mode),
      character_id_(std::move(character_id)),
      checker_(movement.limits, start_of(pb_.setup())),
      saved_(checker_.good()),
      loaded_(checker_.good()),
      sink_(sink),
      save_interval_(movement.save_interval),
      ability_rules_(rules),
      inventory_(std::move(inventory)),
      catalog_(catalog),
      stats_(std::move(character_stats)),
      world_(world),
      class_id_(class_id),
      progress_(progress),
      base_limits_(movement.limits) {}

protocol::emit::FIntrepidNetworkGUID PlayerSession::warehouse_guid(warehouse::Area area) const {
    const WhComponent* first = nullptr;
    for (const WhComponent& w : wh_components_) {
        if (w.key.area != area) continue;
        if (wh_open_ && wh_window_.find(w.key)) return w.comp.guid;
        if (!first) first = &w;
    }
    return first ? first->comp.guid : protocol::emit::FIntrepidNetworkGUID{};
}

std::string url_option(std::string_view url, std::string_view key) {
    size_t pos = 0;
    while (pos <= url.size()) {
        size_t end = url.find('?', pos);
        if (end == std::string_view::npos) end = url.size();
        const std::string_view token = url.substr(pos, end - pos);
        const size_t eq = token.find('=');
        if (eq != std::string_view::npos && iequals(token.substr(0, eq), key)) return std::string(token.substr(eq + 1));
        pos = end + 1;
    }
    return {};
}

bootstrap::LoginIdentity make_login_identity(const std::optional<LoginMessage>& message, const svc::Character& character,
                                             std::vector<std::string>* notes) {
    bootstrap::LoginIdentity id;
    if (message) id.name_option = url_option(message->request_url, "Name");
    if (id.name_option.empty()) {
        id.name_option = character.name;
        if (notes) notes->push_back("the Login has no ?Name= option: the PlayerState's name is the character's");
    }
    if (message && message->has_unique_id && !message->unique_id.empty()) {
        id.unique_id = message->unique_id;
    } else {
        id.unique_id = id.name_option;
        if (notes) notes->push_back("the Login has no unique id of the form the capture's has: the PlayerState's is the name option");
    }
    return id;
}

bool PlayerSession::allocate_channels(const Connection& conn) {
    std::array<bool, 4> done{};
    uint32_t index = 3;   // 0 is the control channel; the original's actors began at 3
    for (size_t role = 0; role < channels_.size(); ++role) {
        for (;; ++index) {
            if (index > 4096) return false;
            if (conn.channel_open(index) || conn.channel_closing(index)) continue;
            break;
        }
        channels_[role] = index++;
        done[role] = true;
    }
    return std::all_of(done.begin(), done.end(), [](bool b) { return b; });
}

PlayerSession::~PlayerSession() {
    clear_pet();
    cancel_quick_make(nullptr, "the session is destroyed");
    cancel_gather(nullptr, "the session is destroyed");
    leave_combat();
    leave_presence("the session is destroyed");
}

uint64_t PlayerSession::SessionInternal::enrich_join_setup(PlayerWorld& world, const svc::Character& character, bootstrap::CharacterSetupResult& built) {
    for (size_t i = 1; i < built.setup->resource_bags.size(); ++i) {
        if (built.setup->resource_bags[i].present && built.setup->guids.resource_storage[i].ObjectId == 0) built.setup->guids.resource_storage[i] = world.mint_guid();
    }
    if (const auto lifted = world.lift_out_of_bowl(built.setup->x, built.setup->y, built.setup->z)) {
        const auto* node = world.city_nodes() ? world.city_nodes()->by_guid(lifted->node) : nullptr;
        spdlog::info("[session] {}: was saved at ({:.1f}, {:.1f}, {:.1f}) cm, {:.0f} m under the floor of a town in a bowl ({}): joins on it, at z {:.1f}", built.setup->name, built.setup->x,
                     built.setup->y, built.setup->z, lifted->under_cm / 100.0, node ? node->display_name : std::to_string(lifted->node), lifted->z);
        built.setup->z = lifted->z;
    }
    uint64_t nodes_version = 0;
    if (world.gamestate_mode() == bootstrap::GameStateMode::Tier3) {
        if (world.city_nodes() && world.node_wire()) {
            nodes_version = world.city_nodes()->version();
            built.setup->nodes = world.node_wire()->maps(*world.city_nodes());
        }
        std::string own = character.id;
        for (char& c : own) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        bool listed_self = false;
        for (const svc::AccountCharacter& other : character.account_characters) {
            bootstrap::AccountCharacterNode n;
            n.guid_hex = other.id;
            for (char& c : n.guid_hex) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            n.name = other.name;
            listed_self = listed_self || n.guid_hex == own;
            built.setup->account_characters.push_back(std::move(n));
        }
        if (!listed_self) {
            bootstrap::AccountCharacterNode n;
            n.guid_hex = own;
            n.name = built.setup->name;
            built.setup->account_characters.insert(built.setup->account_characters.begin(), std::move(n));
        }
    }
    std::vector<int64_t> finished;
    for (const int64_t id : character.completed_ftues) {
        if (id > 0 && finished.size() < bootstrap::kMaxCompletedFtues && std::find(finished.begin(), finished.end(), id) == finished.end()) finished.push_back(id);
    }
    built.setup->completed_ftues = std::move(finished);
    return nodes_version;
}

void PlayerSession::SessionInternal::init_joined_state(PlayerSession& p, PlayerWorld& world, const svc::Character& character,
                                                       bootstrap::CharacterSetupResult& built, uint64_t nodes_version) {
    p.wallet_ = items::Wallet(character.wallet);
    p.jobs_ = character.processing_jobs;
    for (const svc::ProcessingJob& j : p.jobs_) p.job_seen_[j.job_guid] = crafting::status_at(j, p.wall_ms());
    p.sync_active_stations(p.wall_ms());
    p.completed_ftues_ = p.pb_.setup().completed_ftues;
    p.init_quests(world, character);
    if (!p.completed_ftues_.empty()) spdlog::info("[session] {}: {} finished tutorial(s) on the character's list, sent with the join", p.pb_.setup().name, p.completed_ftues_.size());
    p.wh_storage_.load(character.warehouse_items);
    p.init_attunements(character);
    p.taught_ = character.known_recipes;
    if (!p.taught_.empty() && !p.pb_.set_known_recipes(p.taught_)) {
        spdlog::warn("[session] {}: the {} recipes the character was taught do not fit the player's state: the client is not told of them", p.pb_.setup().name, p.taught_.size());
    }
    p.notes_ = std::move(built.notes);
    if (built.items_repaired) {
        p.inventory_dirty_ = true;
        p.inventory_changed_ = true;
    }
    p.professions_ = std::move(built.professions);
    p.weapon_ = std::move(built.weapon);
    if (p.stats_ && p.professions_.valid()) p.stats_->set_profession_levels(p.professions_.levels_by_name());
    p.node_maps_version_ = nodes_version;
    if (const char* env = std::getenv("AOC_PURCHASE_REPLY_DELAY_MS")) {
        char* end = nullptr;
        const long ms = std::strtol(env, &end, 10);
        if (end != env && *end == '\0' && ms >= 0 && ms <= 2000) {
            p.purchase_delay_ = std::chrono::milliseconds(ms);
            spdlog::info("[session] {}: AOC_PURCHASE_REPLY_DELAY_MS = {}: a purchase's reply goes out {} ms after the request", p.pb_.setup().name, ms, ms);
        } else {
            spdlog::warn("[session] {}: AOC_PURCHASE_REPLY_DELAY_MS = '{}' is not 0 to 2000: the default {} ms stays", p.pb_.setup().name, env,
                         kPurchaseReplyDelay.count());
        }
    }
    p.book_ = std::move(built.book);
    p.bar_ = std::move(built.action_bar);
    p.wielded_line_ = built.wielded_line;
    p.known_ = p.pb_.setup().known_abilities;
    for (const bootstrap::AbilityRecord& r : p.pb_.setup().records_to_load) p.records_.push_back(r.guid);
    if (p.book_ && world.content()) {
        if (const auto w = bootstrap::worn_weapon(*world.content(), p.inventory_, items::equip::Primary)) p.combo_ = w->combo;
    }
}

void PlayerSession::SessionInternal::init_mounts_and_view(PlayerSession& p, PlayerWorld& world) {
    p.registry_ = &world.registry();
    p.npc_world_ = &world;
    PlayerSession* self = &p;
    ActorView::Config view_config = world.view_config();
    view_config.player_id = static_cast<uint64_t>(p.pb_.setup().player_id);
    const auto& g = p.pb_.setup().character_guid;
    view_config.player_guid = repl::GuidValue{g[0], g[1], g[2], g[3]};
    view_config.player_name = p.pb_.setup().name;
    if (world.mounts()) {
        MountController::Owner owner;
        owner.player_id = static_cast<uint64_t>(p.pb_.setup().player_id);
        owner.guid = repl::GuidValue{g[0], g[1], g[2], g[3]};
        owner.name = p.pb_.setup().name;
        owner.pawn = p.pb_.setup().guids.pawn;
        owner.server_id = p.pb_.setup().server_id;
        PlayerWorld* w = &world;
        p.mounts_ = std::make_unique<MountController>(world.registry(), *world.mounts(), [w] { return w->mint_guid(); }, std::move(owner));
        if (world.nav() || world.town_platforms()) {
            p.mounts_->set_ground([w](double x, double y, double hint) -> std::optional<double> {
                if (w->bowl_floor_at(x, y)) return w->floor_under(x, y, hint, kMountGroundDownCm, kMountGroundUpCm);
                const nav::AreaNav* nav = w->nav();
                if (!nav) return hint;
                double z = 0;
                if (nav && !nav->ready_at(x, y) && nav->terrain_height(x, y, &z)) return hint;
                if (nav && nav->ground_height(x, y, hint, &z)) return z;
                return std::nullopt;
            });
        }
        const spawns::MountBook* book = world.mounts();
        p.mounts_->set_speeds([book](const std::string& blueprint) -> double {
            const spawns::MountBook::Speeds* s = book->speeds_of(blueprint);
            return s ? s->run : 0.0;
        });
        p.mounts_->set_instance_source([self]() -> int64_t { return self->abilities_ ? self->abilities_->allocate_instance_id() : -1; });
    }
    p.view_ = std::make_unique<ActorView>(world.registry(), view_config, [self](uint32_t index) {
        for (const uint32_t c : self->channels_) {
            if (c == index) return true;
        }
        return false;
    });
}

void PlayerSession::SessionInternal::log_join_summary(PlayerSession& p, PlayerWorld& world) {
    const bootstrap::PlayerSetup& setup = p.pb_.setup();
    spdlog::info("[session] {}: joining as ServerId {}, PlayerId {}, GameState {}; channels PlayerController {}, GameState {}, pawn {}, "
                 "PlayerState {}; at ({:.1f}, {:.1f}, {:.1f}) cm",
                 setup.name, setup.server_id, setup.player_id, mode_name(world.gamestate_mode()), p.channels_[0], p.channels_[1],
                 p.channels_[2], p.channels_[3], setup.x, setup.y, setup.z);
    for (const std::string& note : p.notes_) spdlog::info("[session] {}: {}", setup.name, note);
    if (p.inventory_.size() == 0) {
        spdlog::warn("[session] {}: the character has no items (the character service gives a character its class's starting kit when it starts: is it running a build with items?)", setup.name);
    }
    spdlog::info("[session] {}: level {}, Adventuring {} XP (debt {}), class {}", setup.name, p.progress_.level, p.progress_.xp, p.progress_.debt,
                 bootstrap::client_class_name(p.class_id_) ? bootstrap::client_class_name(p.class_id_) : "?");
    if (p.professions_.valid()) {
        size_t beyond = 0;
        std::string highest;
        int best = 0;
        for (const professions::Profession& pf : p.professions_.book()->all()) {
            const professions::State st = p.professions_.state(pf.guid);
            beyond += st.tier > professions::kTierNovice || st.xp > 0;
            if (st.level > best) {
                best = st.level;
                highest = pf.name + " " + std::to_string(st.level) + " (" + p.professions_.book()->tier(st.tier).name + ")";
            }
        }
        spdlog::info("[session] {}: {} professions, {} beyond Novice level 1 with no XP{}", setup.name, p.professions_.book()->size(), beyond,
                     highest.empty() || best <= 1 ? std::string() : "; the highest is " + highest);
    }
    if (p.weapon_.valid()) {
        std::string lines;
        for (const auto& [type, xp] : p.weapon_.with_xp()) {
            const weaponxp::Line* line = p.weapon_.lines()->find(type);
            lines += (lines.empty() ? "" : ", ") + (line ? line->name : std::to_string(type)) + " level " + std::to_string(p.weapon_.level(type)) + " (" + std::to_string(xp) + " XP)";
        }
        spdlog::info("[session] {}: {} weapon lines, {} with XP{}", setup.name, p.weapon_.lines()->size(), p.weapon_.with_xp().size(), lines.empty() ? std::string() : ": " + lines);
    }
}

std::unique_ptr<PlayerSession> PlayerSession::join(Connection& conn, PlayerWorld& world, const svc::Character& character,
                                                   const bootstrap::LoginIdentity& login, double world_time,
                                                   std::string& error, bootstrap::SetupRefusal* refusal) {
    if (refusal) *refusal = bootstrap::SetupRefusal::Other;
    if (!world.ready()) {
        error = "content.db is not loaded: " + world.content_problem();
        return nullptr;
    }
    const bootstrap::SessionValues values = world.new_session(world_time);
    bootstrap::CharacterSetupResult built = bootstrap::build_player_setup(character, *world.content(), login, values, world.stat_context());
    if (!built.setup) {
        error = built.error;
        if (refusal) *refusal = built.refusal;
        return nullptr;
    }
    const uint64_t nodes_version = SessionInternal::enrich_join_setup(world, character, built);
    std::unique_ptr<PlayerSession> p(
        new PlayerSession(std::move(*built.setup), world.gamestate_mode(), character.id, world.movement(), world.position_sink(),
                          world.ability_rules(), std::move(*built.inventory), world.item_catalog(), std::move(built.character_stats), &world,
                          character.class_id, built.progress));
    SessionInternal::init_joined_state(*p, world, character, built, nodes_version);
    if (!p->pb_.ok()) {
        error = "the bootstrap does not build: " + p->pb_.error();
        return nullptr;
    }
    if (!p->allocate_channels(conn)) {
        error = "no free actor channels";
        return nullptr;
    }
    SessionInternal::init_mounts_and_view(*p, world);
    auto sequence = p->pb_.join_sequence();
    if (!sequence) {
        error = "the join sequence does not build: " + (p->pb_.error().empty() ? std::string("a step failed") : p->pb_.error());
        return nullptr;
    }
    for (bootstrap::Send& s : *sequence) p->join_.push_back(std::move(s));
    SessionInternal::log_join_summary(*p, world);
    p->list_item_chunk(conn);
    p->pump(conn);
    if (refusal) *refusal = bootstrap::SetupRefusal::None;
    return p;
}

spdlog::level::level_enum PlayerSession::level_for(size_t& count, size_t limit) {
    return ++count <= limit ? spdlog::level::info : spdlog::level::debug;
}

void PlayerSession::fail(std::string why) {
    if (failed_) return;
    failed_ = true;
    failure_ = std::move(why);
    spdlog::error("[session] {}: {}", name(), failure_);
}

Outcome PlayerSession::send(Connection& conn, bootstrap::Send& s) {
    Chain chain;
    chain.channel = s.channel != 0 ? s.channel : channels_[static_cast<size_t>(s.role)];
    chain.open = s.opens_channel;
    chain.reliable = s.reliable();
    chain.exports = std::move(s.emission.exports);
    chain.payload = std::move(s.emission.payload);
    if (!conn.can_send_chain(chain)) {
        s.emission.exports = std::move(chain.exports);
        s.emission.payload = std::move(chain.payload);
        if (!chain.reliable || conn.unacked_reliable(chain.channel) == 0) return Outcome::Refused;
        blocked_ = true;   // nothing is built or tried again until an acknowledgement frees some of this channel's window
        blocked_channel_ = chain.channel;
        blocked_unacked_ = conn.unacked_reliable(chain.channel);
        return Outcome::Wait;
    }
    const size_t parts = parts_of(chain);
    const size_t export_parts = chain.exports.size();
    const ChainId id = conn.send_chain(chain);
    if (id == 0) return Outcome::Refused;   // cannot happen after can_send_chain; the Connection logged why
    if (!s.emission.exported_by_bunch.empty()) {
        ChainExports e;
        e.by_bunch = std::move(s.emission.exported_by_bunch);
        e.reliable = chain.reliable;
        e.outstanding = std::min(export_parts, e.by_bunch.size());
        exports_[id] = std::move(e);
    }
    const spdlog::level::level_enum level = !chain.reliable  ? spdlog::level::debug
                                            : !join_.empty() ? spdlog::level::info
                                                             : level_for(reply_sent_logged_);
    spdlog::log(level, "[session] {}: sent {} on channel {} ({} part{}{})", name(), s.what, chain.channel, parts, parts == 1 ? "" : "s",
                chain.reliable ? "" : ", unreliable");
    return Outcome::Sent;
}

void PlayerSession::pump(Connection& conn) {
    if (failed_) return;
    if (blocked_ && conn.unacked_reliable(blocked_channel_) >= blocked_unacked_) return;
    blocked_ = false;
    while (!join_.empty()) {
        bootstrap::Send& head = join_.front();
        switch (send(conn, head)) {
            case Outcome::Sent:
                join_.pop_front();
                waiting_logged_ = false;
                break;
            case Outcome::Wait:
                if (!waiting_logged_) {
                    spdlog::info("[session] {}: the window of channel {} is full: {} chain(s) of the join wait for acknowledgements", name(),
                                 channels_[static_cast<size_t>(head.role)], join_.size());
                    waiting_logged_ = true;
                }
                return;
            case Outcome::Refused:
                fail(std::string("the join cannot be sent: ") + head.what + " was refused by the connection");
                return;
        }
    }
    while (!replies_.empty()) {
        Step& step = replies_.front();
        const bootstrap::PlayerBootstrap::Checkpoint checkpoint = pb_.checkpoint();
        std::optional<bootstrap::Send> built = step.build(pb_);
        if (!built) {
            if (step.must_build) {
                spdlog::warn("[session] {}: {} produced nothing to send: the client gets no reply (the bootstrap says: {})", name(),
                             step.what, pb_.error().empty() ? "nothing changed" : pb_.error());
            } else {
                spdlog::debug("[session] {}: nothing to send for {}", name(), step.what);
            }
            replies_.pop_front();
            continue;
        }
        switch (send(conn, *built)) {
            case Outcome::Sent:
                replies_.pop_front();
                break;
            case Outcome::Wait:
                pb_.restore(checkpoint);
                return;
            case Outcome::Refused:
                pb_.restore(checkpoint);
                spdlog::error("[session] {}: {} was refused by the connection: dropped", name(), step.what);
                replies_.pop_front();
                break;
        }
    }
}

void PlayerSession::tick(Connection& conn) { pump(conn); }

void PlayerSession::tick(Connection& conn, Clock::time_point now) {
    const TickPlan plan = tick_logic(conn, now, /*plan_view=*/false);
    if (plan.stopped) return;
    if (plan.view) view_->tick(conn, plan.x, plan.y, now);
    tick_after(now, plan);
}

bool PlayerSession::position_changed(bool with_yaw) const {
    constexpr double kMovedCm = 1.0;
    constexpr double kTurnedDegrees = 1e-4;
    const GoodPosition g = last_position();
    return std::hypot(std::hypot(g.x - saved_.x, g.y - saved_.y), g.z - saved_.z) > kMovedCm ||
           (with_yaw && std::fabs(g.yaw_degrees - saved_.yaw_degrees) > kTurnedDegrees);
}

GoodPosition PlayerSession::last_position() const {
    GoodPosition g = checker_.good();
    if (mounted()) {
        const Ride::Place p = Ride::rest_place(g.x, g.y, g.z, g.yaw_degrees);
        const double mount_z = g.z;
        g.x = p.x;
        g.y = p.y;
        g.z = p.z;
        if (const auto ground = (g.movement_mode == repl::kMoveFlying || g.movement_mode == repl::kMoveFalling) ? ground_below(p.x, p.y, mount_z) : std::nullopt) {
            if (mount_z - *ground > Ride::kAirborneCm) g.z = *ground + 98.0;
        }
    }
    return g;
}

uint32_t PlayerSession::mount_channel() const { return ride_ && view_ ? view_->channel_of(ride_->actor_id()) : 0; }

void PlayerSession::hand_over(const char* why, bool periodic) {
    if (!sink_) return;
    const GoodPosition g = last_position();
    svc::Position p;
    p.x = g.x;
    p.y = g.y;
    p.z = g.z;
    p.yaw = static_cast<float>(g.yaw_degrees);
    sink_->save(character_id_, p);
    saved_ = g;
    ++handed_over_;
    const spdlog::level::level_enum level = periodic ? level_for(periodic_save_logged_) : spdlog::level::info;
    spdlog::log(level, "[session] {}: saving the last good position ({:.1f}, {:.1f}, {:.1f}) cm, yaw {:.1f} ({})", name(), g.x, g.y, g.z,
                g.yaw_degrees, why);
}

void PlayerSession::finish(const char* why) {
    if (finished_) return;
    finished_ = true;
    clear_pet();
    cancel_quick_make(nullptr, why);   // Crafting, step 3: nothing was taken, so nothing is given back
    end_gathering(why);
    if (registry_) {
        for (const auto& [id, za] : zone_actors_) registry_->remove(za.actor);
    }
    zone_actors_.clear();
    leave_presence(why);
    if (mounts_) {
        if (const auto kept = mounts_->shutdown()) apply_persist(nullptr, *kept);
    }
    leave_combat();
    if (npc_world_) npc_world_->forget_player(static_cast<uint64_t>(pb_.setup().player_id));
    if (view_) {
        spdlog::info("[session] {}: the world's actors: {}", name(), view_->describe());
        view_->finish();
    }
    if (!client_calls_.empty()) {
        std::map<std::string, size_t> by_name;
        for (const auto& [rpc_name, count] : client_calls_) by_name[rpc_name] += count;
        std::vector<std::pair<std::string, size_t>> calls(by_name.begin(), by_name.end());
        std::sort(calls.begin(), calls.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
        std::string text;
        for (size_t i = 0; i < calls.size() && i < 30; ++i) text += (i ? ", " : "") + calls[i].first + " " + std::to_string(calls[i].second);
        spdlog::info("[session] {}: the client's calls by name ({} kinds, the most first): {}{}", name(), calls.size(), text, calls.size() > 30 ? ", ..." : "");
    }
    if (malformed_bunches_ > 0 || in_place_blocks_ > 0 || skipped_fields_ > 0) {
        spdlog::info("[session] {}: the client's calls the reader did not serve: {} bunches that did not read at all (ignored whole); {} calls on subobjects it names by path (skipped, in {} bunches, {} names); {} RPC "
                     "fields of no RPC of their class (skipped)",
                     name(), malformed_bunches_, in_place_blocks_, in_place_bunches_, in_place_names_.size(), skipped_fields_);
    }
    if (abilities_ && !abilities_failed_) {
        const auto held = pb_.current_stats();
        const auto it = held.find(content::kGuidStamina);
        if (it != held.end()) {
            const double server = abilities_->stamina();
            const bool same = static_cast<float>(server) == it->second;
            spdlog::log(same || abilities_dirty_ ? spdlog::level::info : spdlog::level::warn,
                        "[session] {}: Stamina at the end: the server's {:.2f}, the pawn's array {:.2f} ({}{})", name(), server, it->second,
                        same ? "the same" : "DIFFERENT", abilities_dirty_ ? "; an update was still waiting to be built" : "");
        }
    }
    if (quests_on_ && quest_worker_) {
        if (quest_log_.changed() || quests_touched_) quest_worker_->save(character_id_, quest_log_.to_saved());
        quest_worker_->unwatch(character_id_);
    }
    saved_ = loaded_;
    if (handed_over_ > 0 || position_changed(/*with_yaw=*/true)) hand_over(why, /*periodic=*/false);
    if (inventory_changed_) hand_over_inventory(why, /*periodic=*/false);
    if (progress_changed_) hand_over_progress(why);
}

void PlayerSession::hand_over_progress(const char* why) {
    if (!sink_) return;
    svc::Progress p;
    p.level = progress_.level;
    p.class_id = class_id_;
    p.experience.push_back({content::kGuidAdventuring, progress_.xp, progress_.debt});
    for (const svc::Experience& e : weapon_.to_svc()) p.experience.push_back(e);   // weapon XP: the weapon lines' totals (a line with no row is not written)
    p.professions = professions_.to_svc();   // Gathering stage 2: all 22 whole, as the session holds them (empty without the world's book)
    if (book_) {
        p.replace_skill_ranks = true;
        for (const auto& [tree, ranks] : book_->purchased()) p.skill_ranks.push_back({tree, ranks});
        p.replace_action_bar = true;
        for (const repl::ActionbarSlot& slot : bar_.slots()) {
            svc::ActionbarSlot a;
            a.bank = slot.bank_index;
            a.slot = slot.slot_index;
            a.use_type = slot.use.use_type;
            a.instigator = slot.use.instigator;
            a.ability_id = slot.use.ability_id;
            a.skill_rank_id = slot.use.skill_rank_id;
            a.pet_command_id = slot.use.pet_command_id;
            a.item_record_id = slot.use.item_record_id;
            const repl::GuidValue& g = slot.use.item_guid;
            if (g.a | g.b | g.c | g.d) a.item_guid = items::guid_hex(g);
            a.item_rarity = slot.use.item_rarity;
            a.item_quality = slot.use.item_quality;
            a.combo_button = slot.use.combo_button;
            p.action_bar.push_back(std::move(a));
        }
    }
    p.replace_attunements = true;   // Embersprings: the discovered places, the home and its last use go whole with every hand-over
    for (const int n : attunements_.discovered) p.attunements.push_back(n);
    p.home_attunement = attunements_.home;
    p.home_last_used_ms = attunements_.home_last_used_ms;
    p.replace_completed_ftues = true;   // missing systems, step 0: the finished tutorials go whole with every hand-over (what the session holds is what the character has)
    p.completed_ftues = completed_ftues_;
    sink_->save_progress(character_id_, std::move(p));
    ++progress_handed_over_;
    spdlog::info("[session] {}: saving the progress (level {}, class {}, Adventuring {} XP, debt {}) ({})", name(), progress_.level, class_id_, progress_.xp,
                 progress_.debt, why);
}

void PlayerSession::hand_over_inventory(const char* why, bool periodic) {
    inventory_dirty_ = false;
    if (!sink_) return;
    std::vector<svc::InventoryItem> items = inventory_.items();
    const size_t n = items.size();
    sink_->save_inventory(character_id_, std::move(items), wallet_.entries(), jobs_, taught_, wh_storage_.rows());
    ++inventories_handed_over_;
    spdlog::log(periodic ? level_for(item_periodic_logged_) : spdlog::level::info, "[session] {}: saving the inventory ({} items) ({})", name(), n, why);
}

void PlayerSession::queue(const char* what, std::function<std::optional<bootstrap::Send>(bootstrap::PlayerBootstrap&)> build,
                          Connection& conn, bool must_build) {
    if (failed_) return;
    replies_.push_back({what, std::move(build), must_build});
    pump(conn);
}

void PlayerSession::queue_later(const char* what, std::function<std::optional<bootstrap::Send>(bootstrap::PlayerBootstrap&)> build, Connection& conn,
                                Clock::time_point now, bool must_build) {
    if (failed_) return;
    if (purchase_delay_.count() <= 0 && delayed_.empty()) {
        queue(what, std::move(build), conn, must_build);
        return;
    }
    const Clock::time_point due = std::max(now + purchase_delay_, delayed_.empty() ? now : delayed_.back().due);
    delayed_.push_back({due, {what, std::move(build), must_build}});
}

bool PlayerSession::within_budget(RequestBudget& budget, const char* what, Clock::time_point now) {
    if (budget.take(now)) return true;
    if (++dropped_ == 1) {
        spdlog::warn("[session] {}: {} comes faster than its budget allows: dropped (further requests over budget are counted, not logged)",
                     name(), what);
    }
    return false;
}

bool PlayerSession::room_for(size_t steps) {
    if (failed_) return false;
    if (replies_.size() + delayed_.size() + steps <= kMaxPendingReplies) return true;
    if (!replies_full_logged_) {
        spdlog::warn("[session] {}: {} reply steps are waiting for the connection: the client's further requests are ignored", name(),
                     replies_.size() + delayed_.size());
        replies_full_logged_ = true;
    }
    return false;
}

void PlayerSession::take_acks(Connection& conn) {
    for (const AckEvent& e : conn.take_ack_events()) {
        if (view_) view_->on_ack(e);
        const auto it = exports_.find(e.chain);
        if (it == exports_.end() || !e.export_part) continue;
        ChainExports& c = it->second;
        if (e.delivered) {
            if (e.part < c.by_bunch.size()) pb_.mark_exported(c.by_bunch[e.part]);
        } else if (c.reliable) {
            continue;   // the Connection sends the part again and its event follows
        }
        if (c.outstanding > 0 && --c.outstanding == 0) exports_.erase(it);   // a lost unreliable part is final, too
    }
}

void PlayerSession::on_client_load(Connection& conn, Clock::time_point now) {
    if (!within_budget(load_budget_, "ServerRequestClientLoad", now) || !room_for(8)) return;
    spdlog::log(level_for(load_logged_), "[session] {}: the client asks for its load: storage payloads, group and action bar", name());
    std::vector<bootstrap::StorageKind> kinds = {bootstrap::StorageKind::Backpack, bootstrap::StorageKind::Equipment, bootstrap::StorageKind::QuestStorage};
    for (int i = 0; i < items::kResourceContainers; ++i) {
        if (pb_.setup().resource_bags[static_cast<size_t>(i)].present) kinds.push_back(bootstrap::resource_kind(i));   // Gathering stage 3: the container of every worn bag
    }
    for (const bootstrap::StorageKind k : kinds) {
        queue("ClientReceiveStoragePayload", [k](bootstrap::PlayerBootstrap& pb) { return pb.storage_payload(k, Role::PlayerController); },
              conn);
    }
    queue("ClientUpdateGroup", [](bootstrap::PlayerBootstrap& pb) { return pb.empty_group(); }, conn);
    if (book_) {
        const repl::ActionbarPayload payload = bar_payload();
        queue("ClientReceiveActionbarSlotPayload", [payload](bootstrap::PlayerBootstrap& pb) { return pb.action_bar(payload); }, conn);
    } else {
        queue("ClientReceiveActionbarSlotPayload", [](bootstrap::PlayerBootstrap& pb) { return pb.default_action_bar(); }, conn);
    }
}

double PlayerSession::session_seconds(Clock::time_point now) {
    if (!origin_set_) {
        origin_ = now;
        origin_set_ = true;
    }
    return std::chrono::duration<double>(now - origin_).count();
}

void PlayerSession::run_timers(Connection& conn, Clock::time_point now) {
    if (failed_ || finished_ || !join_.empty()) return;   // nothing before the join is on the wire
    while (!delayed_.empty() && delayed_.front().due <= now) {
        Delayed d = std::move(delayed_.front());
        delayed_.pop_front();
        queue(d.step.what, std::move(d.step.build), conn, d.step.must_build);
    }
    if (!markers_.empty()) {
        bootstrap::AbilityChange c;
        for (auto it = markers_.begin(); it != markers_.end();) {
            if (it->due > now) {
                ++it;
                continue;
            }
            c.effects_removed.push_back(it->key);
            for (auto r = rank_effects_.begin(); r != rank_effects_.end();) r = r->second == it->key ? rank_effects_.erase(r) : std::next(r);
            it = markers_.erase(it);
        }
        if (!c.empty()) {
            if (pb_.apply_ability_change(c)) abilities_dirty_ = true;
            else spdlog::warn("[session] {}: the pawn does not take the end of {} marker effect(s) ({})", name(), c.effects_removed.size(), pb_.error().empty() ? "keys disagree" : pb_.error());
        }
    }
    if (abilities_dirty_) flush_abilities(conn);
}

}  // namespace worldd
}  // namespace aoc
