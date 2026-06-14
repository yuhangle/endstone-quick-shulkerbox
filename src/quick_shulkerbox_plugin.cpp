//
// Created by yuhang on 2025/3/20.
//

#include "quick_shulkerbox_plugin.h"
#include "version.h"
#include <endstone_inventoryui/inventoryui.h>
#include <inventoryui_init.h>
#include <ranges>

ENDSTONE_PLUGIN("quick_shulkerbox", PLUGIN_VERSION, QuickShulkerboxPlugin)
{
    description = "Quick Shulkerbox plugin for Endstone servers";
}

void QuickShulkerboxPlugin::onLoad()
{
    getLogger().info("onLoad is called");
}

void QuickShulkerboxPlugin::onEnable()
{
    getLogger().info("onEnable is called");
    inventoryui::initialize_embedded(*this);
    registerEvent<endstone::PlayerInteractEvent>([](const endstone::PlayerInteractEvent& e) {
        onPlayerInteract(e);
    });
}

void QuickShulkerboxPlugin::onDisable()
{
    getLogger().info("onDisable is called");
    inventoryui::shutdown();
}

bool QuickShulkerboxPlugin::onCommand(endstone::CommandSender &sender, const endstone::Command &command, const std::vector<std::string> &args)
{
    return true;
}

void QuickShulkerboxPlugin::onPlayerInteract(const endstone::PlayerInteractEvent &event)
{
    if (event.getAction() != endstone::PlayerInteractEvent::Action::RightClickAir)
        return;

    if (!event.hasItem())
        return;

    const auto &item = event.getItem().value();

    // 匹配所有潜影盒变体
    if (const auto type_id = std::string(item.getType().getId()); type_id.find("shulker_box") == std::string::npos)
        return;

    const auto nbt = item.getNbt();
    if (!nbt.contains("Items") || nbt.at("Items").type() != endstone::nbt::Type::List)
        return;

    const auto &items_list = nbt.at("Items").get<endstone::ListTag>();
    if (items_list.empty())
        return;

    const auto menu = inventoryui::create_menu(inventoryui::MenuTypeId::CHEST,
                                         std::string(item.getType().getId()));
    const auto inv = menu->get_inventory();

    for (const auto &entry_tag : items_list)
    {
        if (entry_tag.type() != endstone::nbt::Type::Compound)
            continue;
        const auto &entry = entry_tag.get<endstone::CompoundTag>();

        // 读取槽位
        int slot = 0;
        if (entry.contains("Slot") && entry.at("Slot").type() == endstone::nbt::Type::Byte)
            slot = entry.at("Slot").get<endstone::ByteTag>().value();

        // 读取物品 ID
        std::string inner_id;
        if (entry.contains("Name") && entry.at("Name").type() == endstone::nbt::Type::String)
            inner_id = entry.at("Name").get<endstone::StringTag>().value();

        // 读取数量
        int count = 1;
        if (entry.contains("Count") && entry.at("Count").type() == endstone::nbt::Type::Byte)
            count = entry.at("Count").get<endstone::ByteTag>().value();

        // 跳过空槽位
        if (inner_id.empty() || count <= 0)
            continue;

        // 校验物品类型有效性
        if (!endstone::ItemType::get(inner_id))
            continue;

        // 构造物品并设置元数据
        endstone::ItemStack stack(inner_id, count);
        if (entry.contains("tag") && entry.at("tag").type() == endstone::nbt::Type::Compound)
            stack.setNbt(entry.at("tag").get<endstone::CompoundTag>());

        if (slot >= 0 && slot < 27)
            inv->set_item(slot, stack);
    }

    // 注册槽位点击监听 — 拿取物品
    menu->set_listener(
        [](const endstone::Player &player, const int slot, const endstone::ItemStack &item1,
           inventoryui::UIInventory &inventory) -> std::function<void()> {
            if (item1.getType() == endstone::ItemType::Air)
                return {};

            auto &playerInv = player.getInventory();
            const int originalAmount = item1.getAmount();
            auto remaining = playerInv.addItem(item1);

            // 计算实际被拿走的数量
            int consumed = originalAmount;
            if (!remaining.empty())
            {
                for (const auto& remainItem : remaining | std::views::values)
                    consumed -= remainItem.getAmount();
            }

            if (consumed <= 0)
            {
                player.sendMessage(endstone::ColorFormat::Red + "你的背包已满，无法取出物品");
                return {};
            }

            // 从玩家主手潜影盒的 NBT 中扣除对应数量
            const int heldSlot = playerInv.getHeldItemSlot();
            if (auto shulkerItem = playerInv.getItem(heldSlot); shulkerItem.has_value())
            {
                if (auto nbt1 = shulkerItem->getNbt(); nbt1.contains("Items") && nbt1.at("Items").type() == endstone::nbt::Type::List)
                {
                    auto &itemsList = nbt1.at("Items").get<endstone::ListTag>();
                    for (std::size_t i = 0; i < itemsList.size(); ++i)
                    {
                        const auto &entry = itemsList.at(i);
                        if (entry.type() != endstone::nbt::Type::Compound)
                            continue;
                        const auto &comp = entry.get<endstone::CompoundTag>();

                        int entrySlot = -1;
                        if (comp.contains("Slot") && comp.at("Slot").type() == endstone::nbt::Type::Byte)
                            entrySlot = comp.at("Slot").get<endstone::ByteTag>().value();

                        std::string entryName;
                        if (comp.contains("Name") && comp.at("Name").type() == endstone::nbt::Type::String)
                            entryName = comp.at("Name").get<endstone::StringTag>().value();

                        if (entrySlot != slot || entryName != std::string(item1.getType().getId()))
                            continue;

                        if (consumed >= originalAmount)
                        {
                            // 全部拿走，删除该条目
                            itemsList.erase(itemsList.begin() + static_cast<long long>(i));
                        }
                        else
                        {
                            // 部分拿走，更新数量 (ByteTag)
                            auto &countTag = const_cast<endstone::nbt::Tag &>(comp.at("Count"));
                            countTag = endstone::ByteTag(static_cast<std::uint8_t>(originalAmount - consumed));
                        }
                        break;
                    }

                    if (itemsList.empty())
                        nbt1.erase("Items");

                    // 写回潜影盒 ItemStack
                    shulkerItem->setNbt(nbt1);
                    playerInv.setItem(heldSlot, shulkerItem);
                }
            }

            // 刷新 UI 显示
            if (consumed >= originalAmount)
                inventory.set_item(slot, endstone::ItemStack("minecraft:air"));
            else
                inventory.set_item(slot, endstone::ItemStack(item1.getType().getId(), originalAmount - consumed));

            return {};
        });

    menu->send_to(event.getPlayer());
}
