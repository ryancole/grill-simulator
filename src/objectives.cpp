#include "objectives.hpp"

#include <utility>

Objectives::Objectives(std::vector<FoodGoal> goals)
    : goals_(std::move(goals)), filled_(goals_.size(), 0) {}

int Objectives::MatchingOpenOrder(std::string_view type, CookInformation::Doneness band) const {
    for (std::size_t i = 0; i < goals_.size(); ++i) {
        const FoodGoal& goal = goals_[i];
        if (filled_[i] >= goal.count) {
            continue; // this order is already met
        }
        if (goal.type != type) {
            continue;
        }
        // The band range is inclusive at both ends; Doneness is an ordered enum, so an
        // undercooked serve is below min and an overcooked one is above max.
        if (band < goal.min || band > goal.max) {
            continue;
        }
        return static_cast<int>(i);
    }
    return -1;
}

bool Objectives::Serve(std::string_view type, CookInformation::Doneness band) {
    const int order = MatchingOpenOrder(type, band);
    if (order < 0) {
        return false;
    }
    ++filled_[static_cast<std::size_t>(order)];
    return true;
}

bool Objectives::WouldAccept(std::string_view type, CookInformation::Doneness band) const {
    return MatchingOpenOrder(type, band) >= 0;
}

const FoodGoal* Objectives::NextOrderFor(std::string_view type) const {
    for (std::size_t i = 0; i < goals_.size(); ++i) {
        if (filled_[i] < goals_[i].count && goals_[i].type == type) {
            return &goals_[i];
        }
    }
    return nullptr;
}

bool Objectives::Complete() const {
    for (std::size_t i = 0; i < goals_.size(); ++i) {
        if (filled_[i] < goals_[i].count) {
            return false;
        }
    }
    return true;
}
