#pragma once

#include "cook_information.hpp"
#include "level.hpp" // FoodGoal

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

// The tracking side of a level's win condition. Built from the level's FoodGoals (its
// order ticket), it counts how many of each order have been delivered and answers
// whether the whole ticket is filled. It is per-level state -- World owns one, rebuilt
// whenever a level loads -- and knows nothing about serve zones or meats: Props decides
// *when* a delivery happens and hands the meat's type and doneness here, and Objectives
// alone decides whether that delivery *counts*.
//
// Wrong deliveries are rejected, not consumed by the tally: an undercooked, overcooked,
// or wrong-type serve fills no order and leaves the counts untouched (the caller keeps
// the meat -- "reject and keep"). Only a serve that lands in an open order's band range
// advances the ticket.
class Objectives {
public:
    Objectives() = default;
    explicit Objectives(std::vector<FoodGoal> goals);

    // Attempt to deliver a meat of `type` cooked to `band`. Returns true if it filled an
    // open order -- one of that type, still wanting another, whose [min, max] band range
    // contains `band` -- and raises that order's filled count by one. Returns false when
    // nothing matched: wrong type, out of band (too rare or too far gone), or every order
    // of that type already filled. The first matching open order is filled.
    bool Serve(std::string_view type, CookInformation::Doneness band);

    // True once every order's count is met -- the level is won. Vacuously true when the
    // level set no goals, so callers that show a win state guard on a non-empty ticket.
    bool Complete() const;

    // The order ticket and how many of each is filled, for a readout. Filled(i) pairs
    // with Goals()[i].
    std::span<const FoodGoal> Goals() const { return goals_; }
    int Filled(std::size_t index) const { return filled_[index]; }

private:
    std::vector<FoodGoal> goals_;
    // One filled-count per goal, index-parallel to goals_.
    std::vector<int> filled_;
};
