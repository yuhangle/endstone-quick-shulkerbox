//
// Created by yuhang on 2025/3/20.
//
#pragma once

#include <endstone/endstone.hpp>
#include <string>

class QuickShulkerboxPlugin : public endstone::Plugin {
public:
    void onLoad() override;

    void onEnable() override;

    void onDisable() override;

    bool onCommand(endstone::CommandSender &sender, const endstone::Command &command, const std::vector<std::string> &args) override;

    static void onPlayerInteract(const endstone::PlayerInteractEvent &event);
};
