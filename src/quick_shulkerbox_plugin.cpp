//
// Created by yuhang on 2025/3/20.
//

#include "quick_shulkerbox_plugin.h"
#include "version.h"
#include <endstone_inventoryui/inventoryui.h>
#include <inventoryui_init.h>
#include <chrono>
#include <unordered_map>

// 辅助：向 ListTag 追加元素，返回新 ListTag
static endstone::ListTag listTagAppend(const endstone::ListTag &src, const endstone::nbt::Tag &value)
{
    endstone::ListTag result;
    for (const auto & i : src)
        result.emplace_back(i);
    result.emplace_back(value);
    return result;
}

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
    // 冷却校验：0.5 秒内不重复触发

    static std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastInteract;
    const auto now = std::chrono::steady_clock::now();
    const auto playerName = event.getPlayer().getName();
    if (auto it = lastInteract.find(playerName); it != lastInteract.end())
    {
        if (std::chrono::duration<double>(now - it->second).count() < 0.5)
            return;
    }
    lastInteract[playerName] = now;

    if (event.getAction() != endstone::PlayerInteractEvent::Action::RightClickAir)
        return;

    if (!event.hasItem())
        return;

    const auto &item = event.getItem().value();

    // 匹配所有潜影盒变体
    if (const auto type_id = std::string(item.getType().getId()); type_id.find("shulker_box") == std::string::npos)
        return;

    const auto nbt = item.getNbt();

    if (nbt.contains("minecraft:item_lock"))
    {
        if (!event.getPlayer().isOp())
        {
            return;
        }
    }

    if (!nbt.contains("Items") || nbt.at("Items").type() != endstone::nbt::Type::List)
        return;

    const auto &items_list = nbt.at("Items").get<endstone::ListTag>();
    if (items_list.empty())
        return;

    auto& server_ = event.getPlayer().getServer();

    std::string menu_title = server_.getLanguage().translate(item.getType().getTranslationKey());

    if (auto meta = item.getItemMeta())
    {
        if (auto name = meta->getDisplayName(); !name.empty())
        {
            menu_title = std::move(name);
        }
    }
    const auto menu = inventoryui::create_menu(inventoryui::MenuTypeId::CHEST,
                                         menu_title);
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
            if (auto remaining = playerInv.addItem(item1); !remaining.empty())
                return {};

            // 从玩家主手潜影盒的 NBT 中移除该物品
            const int heldSlot = playerInv.getHeldItemSlot();

            if (auto shulkerItem = playerInv.getItem(heldSlot); shulkerItem.has_value())
            {
                if (auto nbt1 = shulkerItem->getNbt(); nbt1.contains("Items") && nbt1.at("Items").type() == endstone::nbt::Type::List)
                {
                    const auto &oldList = nbt1.at("Items").get<endstone::ListTag>();
                    endstone::ListTag newList;

                    for (const auto &entry : oldList)
                    {
                        if (entry.type() != endstone::nbt::Type::Compound)
                        {
                            newList.emplace_back(entry);
                            continue;
                        }
                        const auto &comp = entry.get<endstone::CompoundTag>();

                        int entrySlot = -1;
                        if (comp.contains("Slot") && comp.at("Slot").type() == endstone::nbt::Type::Byte)
                            entrySlot = comp.at("Slot").get<endstone::ByteTag>().value();

                        std::string entryName;
                        if (comp.contains("Name") && comp.at("Name").type() == endstone::nbt::Type::String)
                            entryName = comp.at("Name").get<endstone::StringTag>().value();

                        // 匹配则跳过
                        if (entrySlot == slot && entryName == std::string(item1.getType().getId()))
                            continue;

                        newList.emplace_back(entry);
                    }

                    // 使用空列表而非删除键
                    nbt1.insert_or_assign("Items", newList);

                    if (nbt1.contains("minecraft:item_lock"))
                    {
                        // 移除锁定标签以允许 NBT 修改
                        nbt1.erase("minecraft:item_lock");
                    }


                    // 设置修改后的 NBT
                    shulkerItem->setNbt(nbt1);
                    playerInv.setItem(heldSlot, shulkerItem);
                }
            }

            // 清空 UI 槽位
            inventory.set_item(slot, endstone::ItemStack("minecraft:air"));

            return {};
        });

    // 注册玩家物品栏点击监听 — 将物品放入潜影盒
    menu->set_player_inventory_listener(
        [menu](endstone::Player &player, int slot, const endstone::ItemStack &item1,
               int) -> std::function<void()> {
            if (item1.getType() == endstone::ItemType::Air)
                return {};

            auto &playerInv = player.getInventory();

            // 防止嵌套：不接受潜影盒放入潜影盒
            const auto clickedId = std::string(item1.getType().getId());
            if (clickedId.find("shulker_box") != std::string::npos)
            {
                return {};
            }

            // 找到玩家背包中的潜影盒（优先主手）
            int shulkerSlot = playerInv.getHeldItemSlot();
            bool found = false;
            if (auto held = playerInv.getItem(shulkerSlot); held.has_value())
            {
                if (const auto id = std::string(held->getType().getId()); id.find("shulker_box") != std::string::npos)
                    found = true;
            }
            if (!found)
            {
                for (int i = 0; i < 36; ++i)
                {
                    if (auto stack = playerInv.getItem(i); stack.has_value())
                    {
                        if (const auto id = std::string(stack->getType().getId()); id.find("shulker_box") != std::string::npos)
                        {
                            shulkerSlot = i;
                            found = true;
                            break;
                        }
                    }
                }
            }
            if (!found)
                return {};

            auto shulkerItem = playerInv.getItem(shulkerSlot).value();
            auto nbt2 = shulkerItem.getNbt();

            // 确保 Items 列表存在
            endstone::ListTag itemsList;
            if (nbt2.contains("Items") && nbt2.at("Items").type() == endstone::nbt::Type::List)
                itemsList = nbt2.at("Items").get<endstone::ListTag>();

            // 查找潜影盒中是否有同类物品可堆叠，或找第一个空槽位
            const int maxStack = item1.getType().getMaxStackSize();
            int targetSlot = -1;
            bool stacked = false;
            {
                std::vector used(27, false);
                for (const auto & e : itemsList)
                {
                    if (e.type() != endstone::nbt::Type::Compound) continue;
                    const auto &c = e.get<endstone::CompoundTag>();

                    int s = -1;
                    if (c.contains("Slot") && c.at("Slot").type() == endstone::nbt::Type::Byte)
                        s = c.at("Slot").get<endstone::ByteTag>().value();
                    if (s < 0 || s >= 27) continue;
                    used[s] = true;

                    // 找到同类物品且未满最大堆叠数，优先堆叠
                    std::string name;
                    if (c.contains("Name") && c.at("Name").type() == endstone::nbt::Type::String)
                        name = c.at("Name").get<endstone::StringTag>().value();
                    int count = 0;
                    if (c.contains("Count") && c.at("Count").type() == endstone::nbt::Type::Byte)
                        count = c.at("Count").get<endstone::ByteTag>().value();

                    if (name == clickedId && count > 0 && count < maxStack && targetSlot == -1)
                    {
                        targetSlot = s;
                        stacked = true;
                    }
                }

                // 没有可堆叠的同类物品，找第一个空槽
                if (targetSlot == -1)
                {
                    for (int i = 0; i < 27; ++i)
                    {
                        if (!used[i]) { targetSlot = i; break; }
                    }
                }
            }

            // 潜影盒已满（无空槽且无可堆叠槽位）
            if (targetSlot == -1)
                return {};

            // 构建新的 Items 列表，并计算实际放入数量
            int addedAmount = item1.getAmount();
            endstone::ListTag newItemsList;
            for (const auto &e : itemsList)
            {
                if (stacked && e.type() == endstone::nbt::Type::Compound)
                {
                    auto comp = e.get<endstone::CompoundTag>();
                    int s = -1;
                    if (comp.contains("Slot") && comp.at("Slot").type() == endstone::nbt::Type::Byte)
                        s = comp.at("Slot").get<endstone::ByteTag>().value();

                    if (s == targetSlot)
                    {
                        // 堆叠：计算实际可放入数量
                        int oldCount = 1;
                        if (comp.contains("Count") && comp.at("Count").type() == endstone::nbt::Type::Byte)
                            oldCount = comp.at("Count").get<endstone::ByteTag>().value();
                        int space = maxStack - oldCount;
                        addedAmount = std::min(item1.getAmount(), space);
                        int newCount = oldCount + addedAmount;
                        comp.insert_or_assign("Count", endstone::ByteTag(static_cast<std::uint8_t>(newCount)));
                        newItemsList.emplace_back(endstone::nbt::Tag(comp));

                        // 更新 UI 中该槽位的显示
                        endstone::ItemStack updatedStack(clickedId, newCount);
                        if (comp.contains("tag") && comp.at("tag").type() == endstone::nbt::Type::Compound)
                            updatedStack.setNbt(comp.at("tag").get<endstone::CompoundTag>());
                        menu->get_inventory()->set_item(targetSlot, updatedStack);
                        continue;
                    }
                }
                newItemsList.emplace_back(e);
            }

            if (!stacked)
            {
                // 新增条目
                endstone::CompoundTag entry;
                entry.insert_or_assign("Slot", endstone::ByteTag(static_cast<std::uint8_t>(targetSlot)));
                entry.insert_or_assign("Name", endstone::StringTag(clickedId));
                entry.insert_or_assign("Count", endstone::ByteTag(static_cast<std::uint8_t>(item1.getAmount())));
                if (const auto itemNbt = item1.getNbt(); !itemNbt.empty())
                    entry.insert_or_assign("tag", itemNbt);
                newItemsList = listTagAppend(newItemsList, endstone::nbt::Tag(entry));

                // 更新虚拟容器显示
                menu->get_inventory()->set_item(targetSlot, item1);
            }

            // 写回潜影盒
            nbt2.insert_or_assign("Items", newItemsList);

            if (nbt2.contains("minecraft:item_lock"))
            {
                // 移除锁定标签以允许 NBT 修改
                nbt2.erase("minecraft:item_lock");
            }

            // 设置修改后的 NBT
            shulkerItem.setNbt(nbt2);
            playerInv.setItem(shulkerSlot, shulkerItem);

            // 从玩家背包移除已放入的数量
            if (addedAmount >= item1.getAmount())
            {
                // 全部放入，清除槽位
                playerInv.setItem(slot, std::nullopt);
            }
            else
            {
                // 部分放入，减少数量
                int remaining = item1.getAmount() - addedAmount;
                endstone::ItemStack remainingStack(clickedId, remaining);
                if (const auto itemNbt = item1.getNbt(); !itemNbt.empty())
                    remainingStack.setNbt(itemNbt);
                playerInv.setItem(slot, remainingStack);
            }

            // 刷新潜影盒 UI
            menu->refresh_contents(player);

            return {};
        });

    menu->send_to(event.getPlayer());
}
