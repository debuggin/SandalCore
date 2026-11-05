#include "session/player_session.h"

#include "session/gm_travel.h"
#include "session/player_world.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <string>

namespace aoc {
namespace worldd {

namespace repl = ::aoc::protocol::repl;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSamePlaceCm = 1.0;

bool finite3(const repl::Vec3d& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

bool at_place(const repl::Vec3d& p, const GoodPosition& g) {
    return std::fabs(p.x - g.x) <= kSamePlaceCm && std::fabs(p.y - g.y) <= kSamePlaceCm && std::fabs(p.z - g.z) <= kSamePlaceCm;
}

BaseKey key_of_move(const repl::NetworkMove& m) {
    BaseKey k;
    if (m.base_inline) {
        k.object = m.base_inline->outer.ObjectId;
        k.path = m.base_inline->path;
    } else if (m.base) {
        k.object = m.base->ObjectId;
    }
    if (m.base_bone) k.bone = (m.base_bone->hardcoded ? "#" + std::to_string(m.base_bone->index) : m.base_bone->text) + "_" + std::to_string(m.base_bone->number);
    return k;
}

repl::QuantVec quantized_cm(const repl::Vec3d& v) { return repl::quantize_vector(100, v.x, v.y, v.z); }

}  // namespace

struct BasedMoveInternal {
    static void place_or_follow(PlayerSession& s, repl::NetworkMove& m) {
        ++s.moves_based_;
        const bool say = s.moves_based_ <= PlayerSession::kLoggedPerKind || s.moves_based_ % 100 == 0;
        const std::string described = say ? log_text(repl::describe_base(m)) : std::string();
        const char* how = "";
        if (const auto placed = s.place_on_base(m)) {
            m.location = quantized_cm(placed->location);
            m.acceleration = repl::quantize_vector(10, placed->acceleration.x, placed->acceleration.y, placed->acceleration.z);
            s.base_track_.reset();
            s.based_held_run_ = 0;
            how = "placed in the world";
        } else {
            ++s.moves_based_unplaced_;
            const repl::Vec3d offset = m.location_cm();
            const BaseKey key = key_of_move(m);
            const GoodPosition& g = s.checker_.good();
            repl::Vec3d place{g.x, g.y, g.z};
            if (!finite3(offset)) {
                s.base_track_.reset();
                how = "not placed: its offset is not a number";
            } else {
                if (s.base_track_ && s.base_track_->key == key) {
                    place = repl::Vec3d{s.base_track_->place.x + (offset.x - s.base_track_->offset.x),
                                        s.base_track_->place.y + (offset.y - s.base_track_->offset.y),
                                        s.base_track_->place.z + (offset.z - s.base_track_->offset.z)};
                    how = "not placed: followed from the move before on this base";
                } else {
                    ++s.moves_based_held_;
                    ++s.based_held_run_;
                    how = "not placed: held where the player was (the first move on this base)";
                }
                m.location = quantized_cm(place);
                s.base_track_ = BaseTrack{key, offset, m.location_cm()};
            }
        }
        m.base.reset();
        m.base_inline.reset();
        m.base_bone.reset();
        if (say) {
            spdlog::info("[session] {}: a move on a movement base ({}): {} ({} so far, {} not placed)", s.name(), described, how, s.moves_based_, s.moves_based_unplaced_);
        }
    }
};

repl::Vec3d PlayerSession::turn_by_yaw(double yaw_degrees, const repl::Vec3d& v) {
    const double yaw = yaw_degrees * kPi / 180.0;
    const double c = std::cos(yaw), s = std::sin(yaw);
    return repl::Vec3d{c * v.x - s * v.y, s * v.x + c * v.y, v.z};
}

std::optional<PlayerSession::BasedPlace> PlayerSession::place_on_base(const repl::NetworkMove& m) const {
    if (!registry_ || m.base_bone) return std::nullopt;
    const world::WorldActor* a = nullptr;
    if (m.base) {
        a = registry_->find_by_component(m.base->ObjectId);
        if (a && a->npc.collision_cylinder != m.base->ObjectId) a = nullptr;
    } else if (m.base_inline && m.base_inline->path == "CollisionCylinder") {
        a = registry_->find(m.base_inline->outer.ObjectId);
    }
    if (!a) return std::nullopt;
    const repl::Vec3d offset = turn_by_yaw(a->yaw_degrees, m.location_cm());
    BasedPlace out;
    out.location = repl::Vec3d{a->x + offset.x, a->y + offset.y, a->z + offset.z};
    out.acceleration = turn_by_yaw(a->yaw_degrees, m.acceleration_cm());
    if (!finite3(out.location) || !finite3(out.acceleration)) return std::nullopt;
    return out;
}

bool PlayerSession::stand_on_bases(repl::MovePacked& packed) {
    repl::NetworkMove* const moves[3] = {packed.old ? &*packed.old : nullptr, packed.pending ? &*packed.pending : nullptr, &packed.new_move};
    // A base followed from a move that the checker refused (or from before a teleport, a respawn, a mount-up) is not followed any more.
    if (base_track_ && !at_place(base_track_->place, checker_.good())) base_track_.reset();

    for (repl::NetworkMove* m : moves) {
        if (!m) continue;
        if (checker_.is_stale(m->time_stamp)) {
            if (m->has_base()) {
                const GoodPosition& g = checker_.good();
                m->location = quantized_cm(repl::Vec3d{g.x, g.y, g.z});
                m->base.reset();
                m->base_inline.reset();
                m->base_bone.reset();
            }
            continue;
        }
        if (!m->has_base()) {
            base_track_.reset();
            based_held_run_ = 0;
            continue;
        }

        BasedMoveInternal::place_or_follow(*this, *m);

        if (based_held_run_ > kBasedHeldCap) {
            const GoodPosition& g = checker_.good();
            spdlog::log(level_for(moves_based_cut_), "[session] {}: {} moves in a row on movement bases worldd can neither place nor follow: put back at ({:.1f}, {:.1f}, {:.1f}) cm",
                        name(), based_held_run_, g.x, g.y, g.z);
            based_fix_ = pending_fix_ = std::isfinite(packed.new_move.time_stamp) ? packed.new_move.time_stamp : 0.0f;
            held_move_ack_.reset();
            base_track_.reset();
            based_held_run_ = 0;
            return false;
        }
    }
    return true;
}

bool PlayerSession::release_based_fix() {
    if (!based_fix_) return false;
    if (!pending_fix_) pending_fix_ = based_fix_;
    based_fix_.reset();
    return true;
}

std::string PlayerSession::based_move_summary() const {
    std::string s;
    if (moves_unreadable_ > 0) s += ", " + std::to_string(moves_unreadable_) + " unreadable";
    if (moves_based_ > 0) {
        s += ", " + std::to_string(moves_based_) + " on a movement base (" + std::to_string(moves_based_unplaced_) + " not placed";
        if (moves_based_cut_ > 0) s += ", " + std::to_string(moves_based_cut_) + " runs cut by a correction";
        s += ")";
    }
    return s;
}

bool PlayerSession::hold_move_ack(float time_stamp, bool on_mount) {
    if (!world_ || world_->move_ack_gap() <= Clock::duration::zero()) return false;
    if (held_move_ack_) ++move_acks_held_back_;
    held_move_ack_ = HeldMoveAck{time_stamp, on_mount};
    return true;
}

bool PlayerSession::answer_move(Connection& conn, float time_stamp, bool on_mount) {
    auto ack = on_mount ? mount_ack(time_stamp) : pb_.good_move_ack(time_stamp);
    if (!ack) return false;
    if (send(conn, *ack) != Outcome::Sent) return false;
    if (on_mount) ++mount_moves_acked_;
    if (++moves_acked_ == 1) spdlog::info("[session] {}: the first move is acknowledged (TimeStamp {})", name(), time_stamp);
    return true;
}

void PlayerSession::send_move_ack(Connection& conn, Clock::time_point now) {
    if (!held_move_ack_) return;
    const HeldMoveAck held = *held_move_ack_;
    held_move_ack_.reset();
    if (failed_ || held.on_mount != mounted()) return;
    const Clock::duration gap = world_ ? world_->move_ack_gap() : Clock::duration::zero();
    if (move_acked_ && move_acked_on_mount_ == held.on_mount && now - last_move_ack_ <= gap) {
        ++move_acks_held_back_;
        return;
    }
    if (!answer_move(conn, held.time_stamp, held.on_mount)) return;
    move_acked_ = true;
    move_acked_on_mount_ = held.on_mount;
    last_move_ack_ = now;
}

}  // namespace worldd
}  // namespace aoc
