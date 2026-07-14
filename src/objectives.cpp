#include "objectives.hpp"

#include <utility>

Objectives::Objectives(std::vector<FoodGoal> goals)
    : goals_(std::move(goals)), filled_(goals_.size(), 0) {}

bool Objectives::Serve(std::string_view type, CookInformation::Doneness band) {
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
        ++filled_[i];
        return true;
    }
    return false;
}

bool Objectives::Complete() const {
    for (std::size_t i = 0; i < goals_.size(); ++i) {
        if (filled_[i] < goals_[i].count) {
            return false;
        }
    }
    return true;
}
