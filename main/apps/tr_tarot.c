/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：今日塔罗的牌义表和选牌，见 tr_tarot.h。
 *
 * English: The daily tarot meaning table and the daily draw; see tr_tarot.h.
 */

#include "tr_tarot.h"

#include <stdint.h>
#include <time.h>

#include "esp_mac.h"
#include "nvs.h"
#include "tr_time.h"

#define NVS_NS "treader"
#define NVS_KEY "tarot_pick"
#define CARD_COUNT 78

/* ---- 牌义 / Meanings ---- */

// 大阿卡那 0～21，然后权杖、圣杯、宝剑、星币各 14 张（王牌到十、侍从、骑士、王后、国王）。
// Major arcana 0-21, then 14 each of Wands, Cups, Swords and Pentacles (Ace to Ten, Page, Knight, Queen, King).
static const tr_tarot_card_t CARDS[CARD_COUNT] = {
    {"愚者", "The Fool", {"BEGINNINGS · FREEDOM", "HESITATION · RISK"},
     {"新的旅程正在开始，放下包袱。", "冲动之前，先看清脚下的路。"},
     {"迈出第一步就好。", "慢一点再出发。"}},
    {"魔术师", "The Magician", {"WILL · SKILL", "DOUBT · SCATTER"},
     {"你手里已有需要的一切。", "精力分散，事情难有进展。"},
     {"专注做好一件事。", "先理清手头的事。"}},
    {"女祭司", "The High Priestess", {"INTUITION · SILENCE", "NOISE · DOUBT"},
     {"答案在安静处，听听内心。", "外界太吵，你听不清自己。"},
     {"少说多听。", "给自己一点独处。"}},
    {"皇后", "The Empress", {"ABUNDANCE · CARE", "NEGLECT · EXCESS"},
     {"付出的关心正在开花结果。", "照顾别人时，别忘了自己。"},
     {"好好照顾自己。", "留一点给自己。"}},
    {"皇帝", "The Emperor", {"ORDER · STRUCTURE", "RIGIDITY · CONTROL"},
     {"定好规则，事情会更稳。", "抓得太紧，反而失去控制。"},
     {"做个清楚的计划。", "试着放手一点。"}},
    {"教皇", "The Hierophant", {"TRADITION · LEARNING", "REBELLION · NEW PATHS"},
     {"向有经验的人请教会有收获。", "老办法不一定适合现在的你。"},
     {"听听前辈的意见。", "走自己的路。"}},
    {"恋人", "The Lovers", {"CHOICE · HARMONY", "IMBALANCE · INDECISION"},
     {"用心做出选择，关系会更近。", "心意不一，先和自己对话。"},
     {"真诚地表达。", "想清楚再决定。"}},
    {"战车", "The Chariot", {"DRIVE · VICTORY", "DRIFT · DELAY"},
     {"坚定方向，你会冲过难关。", "方向不明，力气用错了地方。"},
     {"一鼓作气。", "先定好目标。"}},
    {"力量", "Strength", {"COURAGE · PATIENCE", "SELF-DOUBT · FATIGUE"},
     {"温柔的坚持比蛮力更有用。", "别怀疑自己，你比想的强。"},
     {"耐心对待难事。", "休息后再试。"}},
    {"隐士", "The Hermit", {"REFLECTION · SOLITUDE", "ISOLATION · WITHDRAWAL"},
     {"独处让你看清方向。", "躲得太久，该出来走走了。"},
     {"读几页书，想一想。", "联系一位老朋友。"}},
    {"命运之轮", "Wheel of Fortune", {"CHANGE · LUCK", "SETBACK · RESISTANCE"},
     {"转机正在到来，顺势而行。", "运气起伏，稳住心态。"},
     {"抓住眼前的机会。", "别急，等一等。"}},
    {"正义", "Justice", {"FAIRNESS · TRUTH", "BIAS · AVOIDANCE"},
     {"公平对待，结果会让人满意。", "有些事还没说清，别回避。"},
     {"实话实说。", "面对该负的责任。"}},
    {"倒吊人", "The Hanged Man", {"PAUSE · NEW VIEW", "STALLING · RESISTANCE"},
     {"停一停，换个角度看问题。", "原地等待太久，该行动了。"},
     {"先放下，不急。", "做个小决定。"}},
    {"死神", "Death", {"ENDING · RENEWAL", "CLINGING · FEAR"},
     {"旧的结束，新的才会开始。", "不肯放手，让你停在原地。"},
     {"清理一件旧事。", "试着告别过去。"}},
    {"节制", "Temperance", {"BALANCE · MODERATION", "EXCESS · HASTE"},
     {"慢慢调和，一切会刚刚好。", "节奏太乱，需要找回平衡。"},
     {"凡事留有余地。", "规律作息。"}},
    {"恶魔", "The Devil", {"TEMPTATION · ATTACHMENT", "RELEASE · CLARITY"},
     {"小心让你上瘾的东西。", "你正在挣脱束缚，继续。"},
     {"少看一会手机。", "坚持好习惯。"}},
    {"高塔", "The Tower", {"UPHEAVAL · REVEAL", "AVERTED · DELAY"},
     {"突然的变化会带来真相。", "危机可以避开，早做准备。"},
     {"保持冷静。", "检查身边的隐患。"}},
    {"星星", "The Star", {"HOPE · FAITH", "DOUBT · WEARINESS"},
     {"保持耐心，新的机会正在靠近。", "暂时失望，希望并没有走远。"},
     {"相信直觉，勇敢尝试。", "给自己一点鼓励。"}},
    {"月亮", "The Moon", {"INTUITION · DREAMS", "CONFUSION · CLARITY"},
     {"事情未明，别急着下结论。", "迷雾正在散去，真相浮现。"},
     {"相信感觉，慢慢看。", "把担心写下来。"}},
    {"太阳", "The Sun", {"JOY · SUCCESS", "DIM · DELAYED JOY"},
     {"阳光明媚，今天适合开心。", "好事稍晚一点，但会来的。"},
     {"和喜欢的人分享。", "保持乐观。"}},
    {"审判", "Judgement", {"AWAKENING · CALLING", "SELF-CRITICISM · DOUBT"},
     {"回顾过去，你会听见新的召唤。", "别对自己太苛刻。"},
     {"原谅过去的自己。", "看到自己的进步。"}},
    {"世界", "The World", {"COMPLETION · WHOLENESS", "UNFINISHED · DELAY"},
     {"一段旅程圆满完成。", "只差最后一步，别放弃。"},
     {"为自己庆祝一下。", "把收尾做好。"}},
    {"权杖王牌", "Ace of Wands", {"INSPIRATION · SPARK", "DELAY · DOUBT"},
     {"灵感闪现，马上动手试试。", "热情不足，计划迟迟不动。"},
     {"把想法记下来。", "先做最小的一步。"}},
    {"权杖二", "Two of Wands", {"PLANNING · VISION", "FEAR · INDECISION"},
     {"眼光放远，规划下一步。", "犹豫太久，机会会溜走。"},
     {"列一张清单。", "今天就定下来。"}},
    {"权杖三", "Three of Wands", {"EXPANSION · PROGRESS", "OBSTACLES · DELAY"},
     {"努力有了回音，继续前进。", "进展慢了点，别着急。"},
     {"保持节奏。", "调整一下计划。"}},
    {"权杖四", "Four of Wands", {"CELEBRATION · HOME", "TENSION · TRANSITION"},
     {"适合和家人朋友聚一聚。", "家里有点小摩擦，多体谅。"},
     {"约人吃顿饭。", "先说声谢谢。"}},
    {"权杖五", "Five of Wands", {"COMPETITION · CLASH", "AVOIDANCE · TRUCE"},
     {"意见不同很正常，别动气。", "争吵渐息，可以好好谈谈。"},
     {"就事论事。", "主动和解。"}},
    {"权杖六", "Six of Wands", {"RECOGNITION · WIN", "EGO · DOUBT"},
     {"你的付出会被看见。", "别太在意别人的评价。"},
     {"大方接受称赞。", "做好自己。"}},
    {"权杖七", "Seven of Wands", {"DEFENSE · RESOLVE", "OVERWHELM · RETREAT"},
     {"守住立场，你有理由坚持。", "压力太大时，可以退一步。"},
     {"别轻易让步。", "挑重要的守。"}},
    {"权杖八", "Eight of Wands", {"SPEED · NEWS", "DELAY · FRUSTRATION"},
     {"消息很快到来，事情加速。", "事情卡住了，耐心等待。"},
     {"及时回复。", "先做别的事。"}},
    {"权杖九", "Nine of Wands", {"RESILIENCE · PERSISTENCE", "EXHAUSTION · DEFENSIVENESS"},
     {"快到终点了，再坚持一下。", "太累了，允许自己休息。"},
     {"别现在放弃。", "早点睡。"}},
    {"权杖十", "Ten of Wands", {"BURDEN · DUTY", "RELIEF · LETTING GO"},
     {"担子太重，试着分出去。", "放下一些，脚步会轻松。"},
     {"请人帮一把。", "删掉一项待办。"}},
    {"权杖侍从", "Page of Wands", {"CURIOSITY · NEWS", "IMPATIENCE · DISTRACTION"},
     {"好奇心会带你发现新东西。", "想法太多，难以落地。"},
     {"学一点新知识。", "一次只做一件。"}},
    {"权杖骑士", "Knight of Wands", {"ADVENTURE · ENERGY", "HASTE · RECKLESSNESS"},
     {"充满干劲，适合出发。", "冲得太快，容易出错。"},
     {"出去走一走。", "做事前检查一遍。"}},
    {"权杖王后", "Queen of Wands", {"CONFIDENCE · WARMTH", "JEALOUSY · INSECURITY"},
     {"自信让你闪闪发光。", "别和别人比较。"},
     {"大方展示自己。", "欣赏自己的长处。"}},
    {"权杖国王", "King of Wands", {"LEADERSHIP · VISION", "IMPULSE · ARROGANCE"},
     {"你有带领大家的能力。", "听听别人，别一意孤行。"},
     {"主动承担一点。", "多问一句。"}},
    {"圣杯王牌", "Ace of Cups", {"LOVE · NEW FEELINGS", "BLOCKED · EMPTY"},
     {"心里涌出新的温暖。", "情绪压着，找个出口。"},
     {"对人说声谢谢。", "写写日记。"}},
    {"圣杯二", "Two of Cups", {"PARTNERSHIP · UNION", "IMBALANCE · TENSION"},
     {"两个人的默契正在加深。", "关系有点失衡，沟通一下。"},
     {"多陪陪身边人。", "好好聊一聊。"}},
    {"圣杯三", "Three of Cups", {"FRIENDSHIP · JOY", "OVERINDULGENCE · GOSSIP"},
     {"朋友会带来快乐。", "热闹过后，留点时间给自己。"},
     {"联系一位好友。", "少说闲话。"}},
    {"圣杯四", "Four of Cups", {"CONTEMPLATION · APATHY", "AWARENESS · NEW INTEREST"},
     {"别只盯着缺的，看看拥有的。", "重新提起兴趣，接受邀请。"},
     {"数一数小幸福。", "答应一次邀请。"}},
    {"圣杯五", "Five of Cups", {"LOSS · REGRET", "ACCEPTANCE · MOVING ON"},
     {"失去的已过去，留下的还在。", "伤心慢慢平复，向前看。"},
     {"看看还剩什么。", "原谅自己。"}},
    {"圣杯六", "Six of Cups", {"NOSTALGIA · KINDNESS", "LIVING IN THE PAST"},
     {"温暖的回忆让人安心。", "别困在过去，活在当下。"},
     {"翻翻旧照片。", "计划一件新事。"}},
    {"圣杯七", "Seven of Cups", {"CHOICES · DREAMS", "CLARITY · FOCUS"},
     {"选择太多，挑最实在的。", "幻想散去，目标更清楚。"},
     {"分清想要和需要。", "专注一个目标。"}},
    {"圣杯八", "Eight of Cups", {"WALKING AWAY · SEARCH", "STAGNATION · FEAR"},
     {"该放下不再适合的东西。", "想走又不敢走，再想想。"},
     {"勇敢说再见。", "听听内心。"}},
    {"圣杯九", "Nine of Cups", {"CONTENTMENT · WISH", "GREED · DISSATISFACTION"},
     {"小小的心愿可能实现。", "知足才会快乐。"},
     {"许个愿吧。", "少买一样东西。"}},
    {"圣杯十", "Ten of Cups", {"HARMONY · FAMILY", "DISCONNECT · STRAIN"},
     {"家的温暖是最好的礼物。", "家人之间需要多一点耐心。"},
     {"给家人打个电话。", "先听对方说。"}},
    {"圣杯侍从", "Page of Cups", {"SENSITIVITY · SURPRISE", "MOODINESS · IMMATURITY"},
     {"一个小惊喜在等你。", "情绪起伏大，先稳一稳。"},
     {"保持开放的心。", "深呼吸三次。"}},
    {"圣杯骑士", "Knight of Cups", {"ROMANCE · INVITATION", "MOODINESS · FANTASY"},
     {"一份邀请或心意正在路上。", "别只凭感觉做决定。"},
     {"回应别人的好意。", "看看实际情况。"}},
    {"圣杯王后", "Queen of Cups", {"COMPASSION · CALM", "OVERWHELM · DEPENDENCE"},
     {"你的体贴让人安心。", "照顾好自己的情绪。"},
     {"倾听别人的心事。", "留点空间给自己。"}},
    {"圣杯国王", "King of Cups", {"BALANCE · DIPLOMACY", "MOODINESS · CONTROL"},
     {"冷静处理，情绪不会失控。", "情绪起伏时先别做决定。"},
     {"温和地坚持。", "等心平再说。"}},
    {"宝剑王牌", "Ace of Swords", {"CLARITY · TRUTH", "CONFUSION · MISJUDGMENT"},
     {"思路清晰，适合做决定。", "想法混乱，先别下判断。"},
     {"说出真实想法。", "收集更多信息。"}},
    {"宝剑二", "Two of Swords", {"STALEMATE · CHOICE", "OVERLOAD · RELEASE"},
     {"两难之间，先停一停。", "信息太多，选一个就好。"},
     {"列出利弊。", "相信第一感觉。"}},
    {"宝剑三", "Three of Swords", {"HEARTBREAK · SORROW", "HEALING · FORGIVENESS"},
     {"难过是正常的，允许自己哭。", "伤口在慢慢愈合。"},
     {"找人倾诉。", "原谅一个人。"}},
    {"宝剑四", "Four of Swords", {"REST · RECOVERY", "RESTLESSNESS · BURNOUT"},
     {"休息是为了走更远。", "再不休息会累坏的。"},
     {"今天早点休息。", "放下工作一会。"}},
    {"宝剑五", "Five of Swords", {"CONFLICT · TENSION", "RECONCILIATION · LESSON"},
     {"赢了争论，可能输了关系。", "争执过去，可以和好了。"},
     {"退一步海阔天空。", "先道个歉。"}},
    {"宝剑六", "Six of Swords", {"TRANSITION · MOVING ON", "RESISTANCE · BAGGAGE"},
     {"离开困境，慢慢变好。", "放不下的事拖住了你。"},
     {"向前看。", "整理一下思绪。"}},
    {"宝剑七", "Seven of Swords", {"STRATEGY · CAUTION", "CONFESSION · CONSCIENCE"},
     {"做事讲方法，也要讲诚信。", "坦白比隐瞒更轻松。"},
     {"留心细节。", "说出实情。"}},
    {"宝剑八", "Eight of Swords", {"RESTRICTION · FEAR", "FREEDOM · NEW VIEW"},
     {"困住你的多是想法本身。", "束缚正在松开，你自由了。"},
     {"换个想法试试。", "迈出一小步。"}},
    {"宝剑九", "Nine of Swords", {"WORRY · ANXIETY", "HOPE · RELIEF"},
     {"担心的事多半不会发生。", "焦虑在减轻，情况在好转。"},
     {"睡前别想太多。", "跟人聊聊。"}},
    {"宝剑十", "Ten of Swords", {"ENDING · LOW POINT", "RECOVERY · RISING"},
     {"最难的时候已经过去。", "从谷底慢慢爬起来。"},
     {"善待自己。", "一步一步来。"}},
    {"宝剑侍从", "Page of Swords", {"CURIOSITY · ALERTNESS", "GOSSIP · HASTE"},
     {"多问多看，会有发现。", "说话前想一想。"},
     {"保持好奇。", "少评论别人。"}},
    {"宝剑骑士", "Knight of Swords", {"AMBITION · ACTION", "IMPULSE · CARELESSNESS"},
     {"目标明确，快速行动。", "急躁容易伤人伤己。"},
     {"说做就做。", "放慢语速。"}},
    {"宝剑王后", "Queen of Swords", {"CLARITY · INDEPENDENCE", "COLDNESS · BITTERNESS"},
     {"清醒独立，看得很透。", "别让过去的伤变成冷漠。"},
     {"直接但温和地说。", "放软一点。"}},
    {"宝剑国王", "King of Swords", {"AUTHORITY · REASON", "HARSHNESS · MISUSE"},
     {"理性判断会帮你解决问题。", "讲理也要讲人情。"},
     {"用事实说话。", "体谅对方。"}},
    {"星币王牌", "Ace of Pentacles", {"OPPORTUNITY · PROSPERITY", "MISSED CHANCE · POOR PLAN"},
     {"一个实际的机会出现了。", "计划不够实际，再算一算。"},
     {"认真把握。", "检查预算。"}},
    {"星币二", "Two of Pentacles", {"BALANCE · ADAPT", "OVERLOAD · DISORDER"},
     {"灵活应对，事事兼顾。", "事情太多，顾此失彼。"},
     {"排好先后顺序。", "推掉一件事。"}},
    {"星币三", "Three of Pentacles", {"TEAMWORK · CRAFT", "DISCORD · SLOPPINESS"},
     {"合作会让事情更好。", "配合不顺，先对齐目标。"},
     {"听听同伴的意见。", "把分工说清楚。"}},
    {"星币四", "Four of Pentacles", {"SECURITY · SAVING", "GREED · RELEASE"},
     {"守住所得，稳中求进。", "握得太紧，反而失去。"},
     {"存一点钱。", "大方一点。"}},
    {"星币五", "Five of Pentacles", {"HARDSHIP · WORRY", "RECOVERY · HELP"},
     {"困难只是暂时的，别怕求助。", "最难的日子正在过去。"},
     {"接受帮助。", "感谢帮过你的人。"}},
    {"星币六", "Six of Pentacles", {"GENEROSITY · SHARING", "DEBT · IMBALANCE"},
     {"给予和接受都是福气。", "付出要量力而行。"},
     {"帮别人一个小忙。", "别勉强自己。"}},
    {"星币七", "Seven of Pentacles", {"PATIENCE · INVESTMENT", "IMPATIENCE · WASTE"},
     {"耕耘还需时间，耐心等待。", "别急着看结果。"},
     {"坚持每天一点。", "调整方法。"}},
    {"星币八", "Eight of Pentacles", {"DILIGENCE · SKILL", "PERFECTIONISM · BOREDOM"},
     {"专注练习，手艺会更好。", "别追求完美，完成更重要。"},
     {"把一件事做精。", "先做完再做好。"}},
    {"星币九", "Nine of Pentacles", {"INDEPENDENCE · REWARD", "OVERWORK · SHALLOW"},
     {"努力换来了自在的生活。", "别只顾工作，生活也重要。"},
     {"犒劳一下自己。", "给生活留时间。"}},
    {"星币十", "Ten of Pentacles", {"LEGACY · STABILITY", "INSTABILITY · LOSS"},
     {"长久的积累带来安稳。", "家里的财务要多留心。"},
     {"想想长远的事。", "记一记账。"}},
    {"星币侍从", "Page of Pentacles", {"LEARNING · FOCUS", "LAZINESS · DELAY"},
     {"适合学一项新技能。", "拖延会让事情更难。"},
     {"开始一门课。", "现在就开始。"}},
    {"星币骑士", "Knight of Pentacles", {"STEADY · DILIGENT", "STUCK · DULL"},
     {"稳扎稳打，必有收获。", "太按部就班，缺少变化。"},
     {"按计划推进。", "换个新方式。"}},
    {"星币王后", "Queen of Pentacles", {"NURTURING · PRACTICAL", "SELF-NEGLECT · WORRY"},
     {"把日子过好，就是成就。", "照顾家人，也要照顾自己。"},
     {"做一顿好吃的。", "给自己放个假。"}},
    {"星币国王", "King of Pentacles", {"SECURITY · SUCCESS", "MATERIALISM · RIGIDITY"},
     {"踏实经营，成果稳定。", "别只看钱，也看人情。"},
     {"守住好习惯。", "关心身边人。"}},
};

/* ---- 选牌 / The draw ---- */

// 日期和设备 MAC 混成一个数：同一天同一台设备不变，不同设备不同。/ Mix the date and the device MAC: fixed for one
// device on one day, different across devices.
static uint32_t day_hash(uint32_t day) {
    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_BASE);
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) h = (h ^ mac[i]) * 16777619u;
    for (int i = 0; i < 4; i++) h = (h ^ ((day >> (8 * i)) & 0xff)) * 16777619u;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    return h;
}

bool tr_tarot_today(tr_tarot_pick_t* out) {
    // 记下的结果：高 24 位是日期 yyyymmdd 减 20000000，低 8 位是牌号乘 2 加逆位。/ The stored result: the date as
    // yyyymmdd minus 20000000 in the top 24 bits, card times 2 plus reversed in the low 8.
    uint32_t saved = 0;
    nvs_handle_t h;
    bool have = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        have = nvs_get_u32(h, NVS_KEY, &saved) == ESP_OK && (saved & 0xff) < CARD_COUNT * 2;
        nvs_close(h);
    }
    if (tr_time_valid()) {
        const time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        const uint32_t day = (uint32_t)(tm.tm_year + 1900) * 10000u + (uint32_t)(tm.tm_mon + 1) * 100u +
                             (uint32_t)tm.tm_mday;
        if (!have || (saved >> 8) != day - 20000000u) {
            const uint32_t hash = day_hash(day);
            saved = (day - 20000000u) << 8 | ((hash % CARD_COUNT) * 2 + ((hash >> 16) & 1));
            have = true;
            if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
                if (nvs_set_u32(h, NVS_KEY, saved) == ESP_OK) nvs_commit(h);
                nvs_close(h);
            }
        }
    }
    if (!have) return false;
    out->index = (int)((saved & 0xff) / 2);
    out->reversed = saved & 1;
    out->card = &CARDS[out->index];
    return true;
}
