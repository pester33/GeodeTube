#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/LazySprite.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>

#include <chrono>
#include <unordered_set>

using namespace geode::prelude;

// Geode 6 (GD 2.2091) and Geode 5 (GD 2.2081) take different time types for timeouts
static void setTimeout(web::WebRequest& req, int secs) {
#if GEODE_COMP_GD_VERSION >= 22091
    req.timeout(asp::Duration::fromSecs(secs));
#else
    req.timeout(std::chrono::seconds(secs));
#endif
}

// Feed built every 2 hours by the "Update video feed" GitHub Action (channels.txt + GD Gaming List)
static constexpr auto FEED_URL = "https://raw.githubusercontent.com/pester33/GeodeTube/main/data/feed.json";

static constexpr ccColor3B COL_BG_DARK = { 24, 24, 24 };
static constexpr ccColor3B COL_CARD = { 39, 39, 39 };
static constexpr ccColor3B COL_CARD_ALT = { 48, 48, 48 };
static constexpr ccColor3B COL_RED = { 230, 33, 23 };
static constexpr ccColor3B COL_MUTED = { 170, 170, 170 };
static constexpr ccColor3B COL_GOLD = { 255, 210, 63 };

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

struct Video {
    std::string id;
    std::string title;
    std::string channel;
    std::string published;
    int64_t views = 0;
    bool isShort = false;
    std::string category; // "latest" or "gdgl"
    int rank = 0;
};

static std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// days since 1970-01-01 for a civil date (Howard Hinnant's algorithm)
static int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = static_cast<unsigned>(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// "2026-10-29T16:00:28+00:00" -> "2 weeks ago"
static std::string timeAgo(std::string const& iso) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (iso.size() < 19 || std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return "";
    int64_t then = daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t diff = std::max<int64_t>(0, now - then);
    auto plural = [](int64_t n, char const* unit) { return fmt::format("{} {}{} ago", n, unit, n == 1 ? "" : "s"); };
    if (diff < 3600) return plural(std::max<int64_t>(1, diff / 60), "minute");
    if (diff < 86400) return plural(diff / 3600, "hour");
    if (diff < 86400 * 7) return plural(diff / 86400, "day");
    if (diff < 86400 * 30) return plural(diff / (86400 * 7), "week");
    if (diff < 86400 * 365) return plural(diff / (86400 * 30), "month");
    return plural(diff / (86400 * 365), "year");
}

// 91096 -> "91K views"
static std::string formatViews(int64_t v) {
    if (v <= 0) return "";
    if (v >= 1'000'000) return fmt::format("{:.1f}M views", v / 1'000'000.0);
    if (v >= 10'000) return fmt::format("{}K views", v / 1000);
    if (v >= 1000) return fmt::format("{:.1f}K views", v / 1000.0);
    return fmt::format("{} views", v);
}

static Result<std::vector<Video>> parseFeed(std::string const& text, std::string& updated) {
    auto parsed = matjson::parse(text);
    if (!parsed) return Err("Feed isn't valid JSON");
    auto json = std::move(parsed).unwrap();
    updated = json["updated"].asString().unwrapOr("");
    auto& videos = json["videos"];
    if (!videos.isArray()) return Err("Feed has no videos");
    std::vector<Video> out;
    for (auto& v : videos) {
        Video vid;
        vid.id = v["id"].asString().unwrapOr("");
        if (vid.id.empty()) continue;
        vid.title = v["title"].asString().unwrapOr("Untitled");
        vid.channel = v["channel"].asString().unwrapOr("");
        vid.published = v["published"].asString().unwrapOr("");
        vid.views = v["views"].asInt().unwrapOr(0);
        vid.isShort = v["short"].asBool().unwrapOr(false);
        vid.category = v["category"].asString().unwrapOr("latest");
        vid.rank = static_cast<int>(v["rank"].asInt().unwrapOr(0));
        out.push_back(std::move(vid));
    }
    return Ok(std::move(out));
}

static void openVideo(Video const& v) {
    auto url = v.isShort ? fmt::format("https://www.youtube.com/shorts/{}", v.id)
                         : fmt::format("https://www.youtube.com/watch?v={}", v.id);
    web::openLinkInBrowser(url);
}

// ---------------------------------------------------------------------------
// UI helpers
// ---------------------------------------------------------------------------

static CCScale9Sprite* panel(CCSize size, ccColor3B color, GLubyte opacity = 255) {
    auto spr = CCScale9Sprite::create("square02b_001.png");
    spr->setColor(color);
    spr->setOpacity(opacity);
    spr->setContentSize(size);
    return spr;
}

// Red rounded "play" logo with a white triangle
static CCNode* playLogo(float width) {
    float height = width * 0.7f;
    auto node = CCNode::create();
    node->setContentSize({ width, height });
    node->setAnchorPoint({ 0.5f, 0.5f });
    auto bg = panel({ width, height }, COL_RED);
    bg->setPosition({ width / 2, height / 2 });
    node->addChild(bg);
    auto tri = CCDrawNode::create();
    float s = height * 0.42f;
    CCPoint pts[3] = {
        { width / 2 - s * 0.45f, height / 2 - s * 0.55f },
        { width / 2 - s * 0.45f, height / 2 + s * 0.55f },
        { width / 2 + s * 0.6f, height / 2 },
    };
    tri->drawPolygon(pts, 3, { 1, 1, 1, 1 }, 0, { 1, 1, 1, 1 });
    node->addChild(tri);
    return node;
}

static CCMenuItemSpriteExtra* textButton(
    char const* text, char const* texture, float scale, std::function<void()> onClick, char const* font = "bigFont.fnt"
) {
    auto spr = ButtonSprite::create(text, font, texture, 0.8f);
    spr->setScale(scale);
    return CCMenuItemExt::createSpriteExtra(spr, [onClick](CCMenuItemSpriteExtra*) { onClick(); });
}

// ---------------------------------------------------------------------------
// The GeodeTube feed popup
// ---------------------------------------------------------------------------

class GeodeTubePopup : public Popup {
protected:
    static constexpr float W = 440.f;
    static constexpr float H = 300.f;
    static constexpr float LIST_W = 412.f;
    static constexpr float LIST_TOP = H - 98.f;
    static constexpr float LIST_BOTTOM = 14.f;
    static constexpr float LIST_H = LIST_TOP - LIST_BOTTOM;
    static constexpr float CARD_H = 82.f;
    static constexpr float THUMB_W = 128.f;
    static constexpr float THUMB_H = 72.f;

    ScrollLayer* m_list = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_subtitle = nullptr;
    CCMenu* m_tabMenu = nullptr;
    std::vector<Video> m_videos;
    std::string m_tab = "latest";
    std::string m_search;
    async::TaskHolder<web::WebResponse> m_task;
    // thumbnails are only downloaded once their card scrolls into view
    std::vector<std::pair<LazySprite*, std::string>> m_pendingThumbs;

    bool init() {
        if (!Popup::init(W, H)) return false;

        // Header
        auto header = panel({ W - 16.f, 40.f }, COL_BG_DARK);
        header->setPosition({ W / 2, H - 28.f });
        m_mainLayer->addChild(header);

        auto logo = playLogo(34.f);
        logo->setPosition({ 50.f, H - 28.f });
        m_mainLayer->addChild(logo);

        auto title = CCLabelBMFont::create("GeodeTube", "bigFont.fnt");
        title->setAnchorPoint({ 0.f, 0.5f });
        title->setScale(0.6f);
        title->setPosition({ 74.f, H - 21.f });
        m_mainLayer->addChild(title);

        m_subtitle = CCLabelBMFont::create("Loading...", "chatFont.fnt");
        m_subtitle->setAnchorPoint({ 0.f, 0.5f });
        m_subtitle->setScale(0.55f);
        m_subtitle->setColor(COL_MUTED);
        m_subtitle->setPosition({ 75.f, H - 37.f });
        m_mainLayer->addChild(m_subtitle);

        auto refresh = textButton("Refresh", "GJ_button_04.png", 0.45f, [this] { this->load(); });
        refresh->setPosition({ W - 50.f, H - 28.f });
        m_buttonMenu->addChild(refresh);

        // Tabs + search
        m_tabMenu = CCMenu::create();
        m_tabMenu->setContentSize({ 230.f, 22.f });
        m_tabMenu->ignoreAnchorPointForPosition(false);
        m_tabMenu->setPosition({ 128.f, H - 64.f });
        m_tabMenu->setLayout(RowLayout::create()->setGap(5.f)->setAxisAlignment(AxisAlignment::Start));
        m_mainLayer->addChild(m_tabMenu);
        for (auto [id, name] : std::vector<std::pair<std::string, std::string>>{
                 { "latest", "Latest" }, { "shorts", "Shorts" }, { "gdgl", "GDGL Records" } }) {
            auto spr = ButtonSprite::create(name.c_str(), "bigFont.fnt", "GJ_button_04.png", 0.8f);
            spr->setScale(0.45f);
            auto btn = CCMenuItemExt::createSpriteExtra(spr, [this, id](CCMenuItemSpriteExtra*) {
                m_tab = id;
                this->rebuild();
            });
            btn->setUserObject(CCString::create(id));
            m_tabMenu->addChild(btn);
        }
        m_tabMenu->updateLayout();

        auto search = TextInput::create(230.f, "Search videos", "bigFont.fnt");
        search->setScale(0.75f);
        search->setPosition({ W - 104.f, H - 64.f });
        search->setCallback([this](std::string const& text) {
            m_search = lower(text);
            this->rebuild();
        });
        m_mainLayer->addChild(search);

        // List
        auto listBg = panel({ LIST_W + 6.f, LIST_H + 6.f }, { 0, 0, 0 }, 90);
        listBg->setPosition({ W / 2, LIST_BOTTOM + LIST_H / 2 });
        m_mainLayer->addChild(listBg);

        m_list = ScrollLayer::create({ LIST_W, LIST_H });
        m_list->setPosition({ W / 2 - LIST_W / 2, LIST_BOTTOM });
        m_list->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout(5.f));
        m_mainLayer->addChild(m_list);

        m_status = CCLabelBMFont::create("Loading...", "bigFont.fnt");
        m_status->setScale(0.5f);
        m_status->setPosition({ W / 2, LIST_BOTTOM + LIST_H / 2 });
        m_mainLayer->addChild(m_status, 5);

        this->schedule(schedule_selector(GeodeTubePopup::loadVisibleThumbs), 0.15f);
        this->load();
        return true;
    }

    void load() {
        m_list->m_contentLayer->removeAllChildren();
        m_pendingThumbs.clear();
        m_status->setString("Loading...");
        m_status->setVisible(true);

        web::WebRequest req;
        setTimeout(req, 20);
        m_task.spawn(req.get(FEED_URL), [this](web::WebResponse res) {
            if (!res.ok()) {
                log::warn("[GeodeTube] Feed failed: HTTP {}", res.code());
                m_status->setString("Couldn't load videos.\nCheck your internet and press Refresh.");
                m_subtitle->setString("Not loaded");
                return;
            }
            std::string updated;
            auto parsed = parseFeed(res.string().unwrapOr(""), updated);
            if (!parsed) {
                log::warn("[GeodeTube] Feed couldn't be read: {}", parsed.unwrapErr());
                m_status->setString("Couldn't read the video feed.");
                return;
            }
            m_videos = std::move(parsed).unwrap();
            auto ago = timeAgo(updated);
            m_subtitle->setString(ago.empty() ? "GD videos" : fmt::format("Feed updated {}", ago).c_str());
            this->rebuild();
        });
    }

    bool matches(Video const& v) const {
        bool shorts = Mod::get()->getSettingValue<bool>("show-shorts");
        if (m_tab == "latest" && (v.category != "latest" || (v.isShort && !shorts))) return false;
        if (m_tab == "shorts" && (v.category != "latest" || !v.isShort)) return false;
        if (m_tab == "gdgl" && v.category != "gdgl") return false;
        if (m_search.empty()) return true;
        return lower(v.title).find(m_search) != std::string::npos || lower(v.channel).find(m_search) != std::string::npos;
    }

    void rebuild() {
        for (auto node : CCArrayExt<CCMenuItemSpriteExtra*>(m_tabMenu->getChildren())) {
            auto id = static_cast<CCString*>(node->getUserObject())->getCString();
            static_cast<ButtonSprite*>(node->getNormalImage())->updateBGImage(m_tab == id ? "GJ_button_02.png" : "GJ_button_04.png");
        }
        auto content = m_list->m_contentLayer;
        content->removeAllChildren();
        m_pendingThumbs.clear();

        int shown = 0;
        for (auto& v : m_videos) {
            if (!this->matches(v)) continue;
            content->addChild(this->createCard(v, shown));
            shown++;
            if (shown >= 120) break;
        }
        if (!m_videos.empty()) {
            m_status->setString(m_search.empty() ? "No videos here yet." : "No videos match your search.");
            m_status->setVisible(shown == 0);
        }
        content->updateLayout();
        m_list->scrollToTop();
        this->loadVisibleThumbs(0.f);
    }

    // Starts downloading thumbnails for cards that are on screen (or about to be)
    void loadVisibleThumbs(float) {
        if (m_pendingThumbs.empty()) return;
        auto listBottom = m_list->convertToWorldSpace({ 0, 0 }).y;
        auto listTop = m_list->convertToWorldSpace({ 0, LIST_H }).y;
        float margin = (listTop - listBottom) * 0.5f;
        for (auto it = m_pendingThumbs.begin(); it != m_pendingThumbs.end();) {
            auto [spr, url] = *it;
            float y = spr->convertToWorldSpace({ 0, 0 }).y;
            if (y > listBottom - margin - THUMB_H && y < listTop + margin) {
                spr->loadFromUrl(url);
                it = m_pendingThumbs.erase(it);
            } else {
                ++it;
            }
        }
    }

    CCNode* createCard(Video const& v, int index) {
        auto card = CCNode::create();
        card->setContentSize({ LIST_W, CARD_H });

        auto bg = panel({ LIST_W - 4.f, CARD_H }, index % 2 ? COL_CARD_ALT : COL_CARD);
        bg->setPosition({ LIST_W / 2, CARD_H / 2 });
        card->addChild(bg);

        // Thumbnail
        float thumbX = 8.f + THUMB_W / 2;
        auto thumbBg = panel({ THUMB_W, THUMB_H }, { 15, 15, 15 });
        thumbBg->setPosition({ thumbX, CARD_H / 2 });
        card->addChild(thumbBg);
        auto thumb = LazySprite::create({ THUMB_W, THUMB_H });
        thumb->setAutoResize(true);
        thumb->setPosition({ thumbX, CARD_H / 2 });
        card->addChild(thumb);
        m_pendingThumbs.push_back({ thumb, fmt::format("https://i.ytimg.com/vi/{}/mqdefault.jpg", v.id) });

        if (v.isShort) {
            auto tag = panel({ 40.f, 13.f }, COL_RED);
            tag->setPosition({ 8.f + THUMB_W - 24.f, CARD_H / 2 - THUMB_H / 2 + 10.f });
            card->addChild(tag, 2);
            auto tagLbl = CCLabelBMFont::create("SHORT", "bigFont.fnt");
            tagLbl->setScale(0.28f);
            tagLbl->setPosition(tag->getPosition());
            card->addChild(tagLbl, 3);
        }
        if (v.category == "gdgl" && v.rank > 0 && v.rank <= 3) {
            ccColor3B medal = v.rank == 1 ? COL_GOLD : v.rank == 2 ? ccColor3B{ 207, 214, 222 } : ccColor3B{ 216, 137, 74 };
            auto tag = panel({ 26.f, 14.f }, medal);
            tag->setPosition({ 8.f + 17.f, CARD_H / 2 + THUMB_H / 2 - 10.f });
            card->addChild(tag, 2);
            auto lbl = CCLabelBMFont::create(fmt::format("#{}", v.rank).c_str(), "bigFont.fnt");
            lbl->setScale(0.3f);
            lbl->setColor({ 40, 25, 10 });
            lbl->setPosition(tag->getPosition());
            card->addChild(lbl, 3);
        }

        // Title (wraps to two lines), channel, views and age
        float textX = 8.f + THUMB_W + 10.f;
        float textW = LIST_W - textX - 74.f;
        float titleScale = 0.62f;
        std::string title = v.title.size() > 90 ? v.title.substr(0, 87) + "..." : v.title;
        auto titleLbl = CCLabelBMFont::create(title.c_str(), "chatFont.fnt", textW / titleScale, kCCTextAlignmentLeft);
        titleLbl->setAnchorPoint({ 0.f, 1.f });
        titleLbl->setScale(titleScale);
        titleLbl->setPosition({ textX, CARD_H - 10.f });
        card->addChild(titleLbl);

        auto channelLbl = CCLabelBMFont::create(v.channel.c_str(), "chatFont.fnt");
        channelLbl->setAnchorPoint({ 0.f, 0.5f });
        channelLbl->limitLabelWidth(textW, 0.55f, 0.2f);
        channelLbl->setColor(COL_MUTED);
        channelLbl->setPosition({ textX, 22.f });
        card->addChild(channelLbl);

        std::vector<std::string> meta;
        if (auto views = formatViews(v.views); !views.empty()) meta.push_back(views);
        if (auto ago = timeAgo(v.published); !ago.empty()) meta.push_back(ago);
        if (v.category == "gdgl") meta.push_back("GD Gaming List record");
        std::string metaText;
        for (size_t i = 0; i < meta.size(); i++) metaText += (i ? "  -  " : "") + meta[i];
        auto metaLbl = CCLabelBMFont::create(metaText.c_str(), "chatFont.fnt");
        metaLbl->setAnchorPoint({ 0.f, 0.5f });
        metaLbl->limitLabelWidth(textW, 0.5f, 0.2f);
        metaLbl->setColor(COL_MUTED);
        metaLbl->setPosition({ textX, 10.f });
        card->addChild(metaLbl);

        // Watch button (opens YouTube)
        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        menu->setContentSize(card->getContentSize());
        card->addChild(menu);
        auto vid = v;
        auto watch = textButton("Watch", "GJ_button_06.png", 0.5f, [vid] { openVideo(vid); });
        watch->setPosition({ LIST_W - 38.f, CARD_H / 2 });
        menu->addChild(watch);

        return card;
    }

public:
    static GeodeTubePopup* create() {
        auto ret = new GeodeTubePopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// Self-updater (same as the other mods)
// ---------------------------------------------------------------------------

#if GEODE_COMP_GD_VERSION >= 22091
static constexpr auto RELEASE_BASE = "https://github.com/pester33/GeodeTube/releases/download/latest-build-2.2091/";
#else
static constexpr auto RELEASE_BASE = "https://github.com/pester33/GeodeTube/releases/download/latest-build/";
#endif

static async::TaskHolder<web::WebResponse> s_updateTask;
static bool s_updateBusy = false;

static void whenScreenReady(std::function<void()> fn, int tries = 0) {
    auto scene = CCDirector::sharedDirector()->getRunningScene();
    bool ready = scene && !typeinfo_cast<CCTransitionScene*>(scene);
    if (ready || tries > 600) {
        fn();
        return;
    }
    Loader::get()->queueInMainThread([fn = std::move(fn), tries]() mutable {
        whenScreenReady(std::move(fn), tries + 1);
    });
}

static void installUpdate(std::string version) {
    if (s_updateBusy) return;
    s_updateBusy = true;
    auto url = fmt::format("{}{}.geode", RELEASE_BASE, Mod::get()->getID());
    log::info("[updater] Downloading {} from {}", version, url);
    auto loading = Notification::create("Downloading GeodeTube update...", NotificationIcon::Loading, 0.f);
    loading->show();

    web::WebRequest req;
    setTimeout(req, 90);
    s_updateTask.spawn(req.get(url), [loading = Ref(loading), version](web::WebResponse res) {
        s_updateBusy = false;
        loading->hide();
        auto const& data = res.data();
        log::info("[updater] Download finished: HTTP {}, {} bytes", res.code(), data.size());
        if (!res.ok() || data.size() < 4 || data[0] != 'P' || data[1] != 'K') {
            Notification::create(fmt::format("Update failed to download (HTTP {})", res.code()), NotificationIcon::Error, 3.f)->show();
            return;
        }
        auto written = file::writeBinarySafe(Mod::get()->getPackagePath(), data);
        if (!written) {
            log::warn("[updater] Couldn't write the mod file: {}", written.unwrapErr());
            Notification::create("Update failed to install", NotificationIcon::Error, 3.f)->show();
            return;
        }
        whenScreenReady([version] {
            createQuickPopup(
                "Update installed",
                fmt::format("GeodeTube <cg>{}</c> is installed.\nRestart GD to use it.", version),
                "Later", "Restart",
                [](auto, bool restart) { if (restart) game::restart(true); }
            );
        });
    });
}

static void checkForUpdate(bool isRetry = false) {
    if (s_updateBusy) return;
    s_updateBusy = true;
    auto url = fmt::format("{}version.txt", RELEASE_BASE);
    log::info("[updater] Checking {} (current {})", url, Mod::get()->getVersion().toVString());

    web::WebRequest req;
    setTimeout(req, 20);
    s_updateTask.spawn(req.get(url), [isRetry](web::WebResponse res) {
        s_updateBusy = false;
        auto body = utils::string::trim(res.string().unwrapOr(""));
        log::info("[updater] version.txt: HTTP {}, body '{}'", res.code(), body.substr(0, 40));
        if (!res.ok()) {
            if (!isRetry) Loader::get()->queueInMainThread([] { checkForUpdate(true); });
            return;
        }
        auto latest = VersionInfo::parse(body);
        if (!latest) return;
        auto current = Mod::get()->getVersion();
        if (!(latest.unwrap() > current)) return;
        auto version = latest.unwrap().toVString();
        whenScreenReady([version, current] {
            createQuickPopup(
                "GeodeTube",
                fmt::format("A new version is out: <cg>{}</c> (you have {}).\nUpdate now?", version, current.toVString()),
                "Later", "Update",
                [version](auto, bool update) { if (update) installUpdate(version); }
            );
        });
    });
}

// ---------------------------------------------------------------------------
// Main menu button
// ---------------------------------------------------------------------------

class $modify(GeodeTubeMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        static bool firstTime = true;
        if (firstTime) {
            firstTime = false;
            if (Mod::get()->getSettingValue<bool>("check-updates")) checkForUpdate();
        }

        if (!Mod::get()->getSettingValue<bool>("menu-button")) return true;

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        auto spr = CCNode::create();
        spr->setContentSize({ 64.f, 52.f });
        auto logo = playLogo(40.f);
        logo->setPosition({ 32.f, 32.f });
        spr->addChild(logo);
        auto label = CCLabelBMFont::create("GeodeTube", "goldFont.fnt");
        label->limitLabelWidth(64.f, 0.45f, 0.2f);
        label->setPosition({ 32.f, 6.f });
        spr->addChild(label);

        auto btn = CCMenuItemExt::createSpriteExtra(spr, [](CCMenuItemSpriteExtra*) {
            if (auto popup = GeodeTubePopup::create()) popup->show();
        });
        btn->setID("geodetube-button"_spr);
        auto menu = CCMenu::create();
        menu->setID("geodetube-menu"_spr);
        menu->setPosition({ 0.f, 0.f });
        btn->setPosition({ 40.f, winSize.height / 2.f + 20.f });
        menu->addChild(btn);
        this->addChild(menu, 10);
        return true;
    }
};
