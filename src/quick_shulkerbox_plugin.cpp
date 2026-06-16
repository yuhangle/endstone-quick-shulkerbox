//
// Created by yuhang on 2025/3/20.
//

#include "quick_shulkerbox_plugin.h"
#include "version.h"
#include <endstone_inventoryui/inventoryui.h>
#include <inventoryui_init.h>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

// 不允许通过快速潜影盒操作的物品黑名单（关键词匹配变种）
// bundle: 收纳袋（内含物品 NBT 会丢失）
// potion: 药水（效果数据不在 ItemStack NBT 中）
// ominous_bottle: 不详之瓶（同理）
static constexpr std::string_view kBlacklistedItems[] = {
    "bundle",
    "potion",
    "ominous_bottle",
};

//储存翻译语句
namespace trans
{
    std::string ps = "[QuickShulkerBox] ";
    std::string ps_chs = "[快速潜影盒] ";
    std::string chs = "为了保护你的数据，部分物品不能被操作";
    std::string eng = "To protect your data,some items cannot be stored or retrieved.";

}

static bool isBlacklisted(const std::string &typeId)
{
    return std::ranges::any_of(kBlacklistedItems, [&typeId](std::string_view keyword) {
        return typeId.find(keyword) != std::string::npos;
    });
}

// 辅助：比较两个 CompoundTag 是否内容相等
static bool compoundTagEqual(const endstone::CompoundTag &a, const endstone::CompoundTag &b)
{
    if (a.size() != b.size()) return false;
    return std::ranges::all_of(a, [&b](const auto &entry) {
        return b.contains(entry.first) && b.at(entry.first) == entry.second;
    });
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

    // 每 60 秒清理过期条目
    {
        static auto lastCleanup = now;
        if (std::chrono::duration<double>(now - lastCleanup).count() > 60.0)
        {
            lastCleanup = now;
            for (auto it = lastInteract.begin(); it != lastInteract.end();)
            {
                if (std::chrono::duration<double>(now - it->second).count() > 60.0)
                    it = lastInteract.erase(it);
                else
                    ++it;
            }
        }
    }

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

    endstone::ListTag items_list;
    if (nbt.contains("Items") && nbt.at("Items").type() == endstone::nbt::Type::List)
        items_list = nbt.at("Items").get<endstone::ListTag>();

    auto& server_ = event.getPlayer().getServer();

    std::string menu_title = server_.getLanguage().translate(item.getType().getTranslationKey(), event.getPlayer().getLocale());

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

    // 捕获潜影盒槽位，用于 listener 中校验是否仍为潜影盒
    const int capturedShulkerSlot = event.getPlayer().getInventory().getHeldItemSlot();

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

    // 黑名单物品点击标记（共享给两个 listener 和关闭回调）
    auto blacklistedClickers = std::make_shared<std::unordered_set<std::string>>();

    // 注册槽位点击监听 — 拿取物品
    menu->set_listener(
        [capturedShulkerSlot, blacklistedClickers](const endstone::Player &player, const int slot, const endstone::ItemStack &item1,
           inventoryui::UIInventory &inventory) -> std::function<void()> {
            if (item1.getType() == endstone::ItemType::Air)
                return {};

            // 黑名单物品不允许取出
            if (isBlacklisted(std::string(item1.getType().getId())))
            {
                blacklistedClickers->insert(player.getName());
                return {};
            }

            auto &playerInv = player.getInventory();
            if (auto remaining = playerInv.addItem(item1); !remaining.empty())
                return {};

            // 校验：捕获槽位中仍然是潜影盒
            auto shulkerOpt = playerInv.getItem(capturedShulkerSlot);
            if (!shulkerOpt.has_value())
                return {};
            if (std::string(shulkerOpt->getType().getId()).find("shulker_box") == std::string::npos)
                return {};
            auto shulkerItem = shulkerOpt.value();

            if (auto nbt1 = shulkerItem.getNbt(); nbt1.contains("Items") && nbt1.at("Items").type() == endstone::nbt::Type::List)
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

                    int entryCount = 0;
                    if (comp.contains("Count") && comp.at("Count").type() == endstone::nbt::Type::Byte)
                        entryCount = comp.at("Count").get<endstone::ByteTag>().value();

                    // 匹配 slot + name + count + tag 才移除
                    if (entrySlot == slot && entryName == std::string(item1.getType().getId())
                        && entryCount == item1.getAmount())
                    {
                        // 额外比较 tag（NBT 内容）
                        bool tagMatch = true;
                        const auto itemNbt = item1.getNbt();
                        if (comp.contains("tag") && comp.at("tag").type() == endstone::nbt::Type::Compound)
                        {
                            if (itemNbt.empty())
                                tagMatch = false;
                            else
                                tagMatch = compoundTagEqual(comp.at("tag").get<endstone::CompoundTag>(), itemNbt);
                        }
                        else if (!itemNbt.empty())
                        {
                            tagMatch = false;
                        }
                        if (tagMatch)
                            continue;
                    }

                    newList.emplace_back(entry);
                }

                // 使用空列表而非删除键
                nbt1.insert_or_assign("Items", newList);

                // 临时移除锁定标签以允许 NBT 修改，操作后恢复
                std::optional<endstone::nbt::Tag> savedLock;
                if (nbt1.contains("minecraft:item_lock"))
                {
                    savedLock = nbt1.at("minecraft:item_lock");
                    nbt1.erase("minecraft:item_lock");
                }

                // 设置修改后的 NBT
                shulkerItem.setNbt(nbt1);

                // 恢复 item_lock 标签
                if (savedLock)
                {
                    auto finalNbt = shulkerItem.getNbt();
                    finalNbt.insert_or_assign("minecraft:item_lock", *savedLock);
                    shulkerItem.setNbt(finalNbt);
                }

                playerInv.setItem(capturedShulkerSlot, shulkerItem);
            }

            // 清空 UI 槽位
            inventory.set_item(slot, endstone::ItemStack("minecraft:air"));

            return {};
        });

    // 注册玩家物品栏点击监听 — 将物品放入潜影盒
    menu->set_player_inventory_listener(
        [menu, capturedShulkerSlot, blacklistedClickers](endstone::Player &player, int slot, const endstone::ItemStack &item1,
               int) -> std::function<void()> {
            if (item1.getType() == endstone::ItemType::Air)
                return {};

            auto &playerInv = player.getInventory();

            const auto clickedId = std::string(item1.getType().getId());

            // 黑名单物品不允许存入
            if (isBlacklisted(clickedId))
            {
                blacklistedClickers->insert(player.getName());
                return {};
            }

            // 防止嵌套：不接受潜影盒放入潜影盒
            if (clickedId.find("shulker_box") != std::string::npos)
                return {};

            // 校验：捕获槽位中仍然是潜影盒
            auto shulkerOpt = playerInv.getItem(capturedShulkerSlot);
            if (!shulkerOpt.has_value())
                return {};
            if (std::string(shulkerOpt->getType().getId()).find("shulker_box") == std::string::npos)
                return {};
            auto shulkerItem = shulkerOpt.value();
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

                    // 堆叠条件：同名 + 未满 + tag 一致
                    if (name == clickedId && count > 0 && count < maxStack && targetSlot == -1)
                    {
                        // 比较 tag（NBT 内容），确保相同附魔/命名的物品才堆叠
                        bool tagMatch = true;
                        const auto itemNbt = item1.getNbt();
                        if (c.contains("tag") && c.at("tag").type() == endstone::nbt::Type::Compound)
                        {
                            if (itemNbt.empty())
                                tagMatch = false;
                            else
                                tagMatch = compoundTagEqual(c.at("tag").get<endstone::CompoundTag>(), itemNbt);
                        }
                        else if (!itemNbt.empty())
                        {
                            tagMatch = false;
                        }
                        if (tagMatch)
                        {
                            targetSlot = s;
                            stacked = true;
                        }
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

            // 防御：无实际放入量时不修改潜影盒
            if (addedAmount <= 0)
                return {};

            if (!stacked)
            {
                // 自行构造完整 CompoundTag，包含 Bedrock 内部字段
                endstone::CompoundTag entry;
                entry.insert_or_assign("Slot", endstone::ByteTag(static_cast<std::uint8_t>(targetSlot)));
                entry.insert_or_assign("Name", endstone::StringTag(clickedId));
                entry.insert_or_assign("Count", endstone::ByteTag(static_cast<std::uint8_t>(item1.getAmount())));
                entry.insert_or_assign("Damage", endstone::ShortTag(0));
                entry.insert_or_assign("WasPickedUp", endstone::ByteTag(0));
                if (const auto itemNbt = item1.getNbt(); !itemNbt.empty())
                    entry.insert_or_assign("tag", itemNbt);
                newItemsList.emplace_back(endstone::nbt::Tag(entry));

                // 更新虚拟容器显示
                menu->get_inventory()->set_item(targetSlot, item1);
            }

            // 按 Slot 号排序，确保 list 顺序与原始一致
            std::ranges::sort(newItemsList,
                              [](const endstone::nbt::Tag &a, const endstone::nbt::Tag &b) {
                                  auto getSlot = [](const endstone::nbt::Tag &t) -> int {
                                      if (t.type() != endstone::nbt::Type::Compound) return -1;
                                      const auto &c = t.get<endstone::CompoundTag>();
                                      if (c.contains("Slot") && c.at("Slot").type() == endstone::nbt::Type::Byte)
                                          return c.at("Slot").get<endstone::ByteTag>().value();
                                      return -1;
                                  };
                                  return getSlot(a) < getSlot(b);
                              });

            // 去重：同一槽位只保留最后一个条目
            {
                std::vector<int> lastIdx(27, -1);
                for (int i = 0; i < static_cast<int>(newItemsList.size()); ++i)
                {
                    const auto &e = newItemsList[i];
                    if (e.type() != endstone::nbt::Type::Compound) continue;
                    const auto &c = e.get<endstone::CompoundTag>();
                    int s = -1;
                    if (c.contains("Slot") && c.at("Slot").type() == endstone::nbt::Type::Byte)
                        s = c.at("Slot").get<endstone::ByteTag>().value();
                    if (s >= 0 && s < 27)
                        lastIdx[s] = i;
                }
                endstone::ListTag deduped;
                for (int i = 0; i < static_cast<int>(newItemsList.size()); ++i)
                {
                    const auto &e = newItemsList[i];
                    if (e.type() != endstone::nbt::Type::Compound)
                    {
                        deduped.emplace_back(e);
                        continue;
                    }
                    const auto &c = e.get<endstone::CompoundTag>();
                    int s = -1;
                    if (c.contains("Slot") && c.at("Slot").type() == endstone::nbt::Type::Byte)
                        s = c.at("Slot").get<endstone::ByteTag>().value();
                    if (s >= 0 && s < 27)
                    {
                        if (i == lastIdx[s])
                            deduped.emplace_back(e);
                    }
                    else
                    {
                        deduped.emplace_back(e);
                    }
                }
                newItemsList = std::move(deduped);
            }

            // 写回潜影盒
            nbt2.insert_or_assign("Items", newItemsList);

            // 临时移除锁定标签以允许 NBT 修改，操作后恢复
            std::optional<endstone::nbt::Tag> savedLock2;
            if (nbt2.contains("minecraft:item_lock"))
            {
                savedLock2 = nbt2.at("minecraft:item_lock");
                nbt2.erase("minecraft:item_lock");
            }

            // 设置修改后的 NBT
            shulkerItem.setNbt(nbt2);

            // 恢复 item_lock 标签
            if (savedLock2)
            {
                auto finalNbt = shulkerItem.getNbt();
                finalNbt.insert_or_assign("minecraft:item_lock", *savedLock2);
                shulkerItem.setNbt(finalNbt);
            }

            playerInv.setItem(capturedShulkerSlot, shulkerItem);

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

    // 关闭 UI 时，提示点击过黑名单物品的玩家
    menu->set_close_listener([blacklistedClickers](const endstone::Player &player) {
        if (blacklistedClickers->contains(player.getName()))
        {
            if (player.getLocale() == "zh_CN")
            {
                player.sendErrorMessage(trans::ps_chs + trans::chs);
            } else
            {
                player.sendErrorMessage(trans::ps + trans::eng);
            }
            blacklistedClickers->erase(player.getName());
        }
    });

    menu->send_to(event.getPlayer());
}
