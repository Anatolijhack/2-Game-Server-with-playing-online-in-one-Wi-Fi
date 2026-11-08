#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include "Registry.h"
#include "Components.h"
#include "System.h"
#include "GameSystems.h"
#include "Game.h"          // ваш: GameState, UpgradeKind, applyUpgrade, evaluateState, resetWorld. Без SFML!
#include "Protocol.h"

// Серверная копия логики GameScene + WaveClearScene из вашего main.cpp, без окна и рендера.
// Системы, компоненты и функции игры используются как есть. Один Instance = мир одного игрока.

constexpr int   TICK_RATE = 30;
constexpr float DT = 1.0f / TICK_RATE;
constexpr float ARENA_W = 800.f;     // как в SceneManager и BoundsSystem
constexpr float ARENA_H = 600.f;

inline std::int16_t q16(float v)
{
    return static_cast<std::int16_t>(std::lround(std::clamp(v, -32768.f, 32767.f)));
}

inline std::uint8_t t255(float life, float maxLife)
{
    if (maxLife <= 0.f) return 0;
    return static_cast<std::uint8_t>(std::clamp(life / maxLife, 0.f, 1.f) * 255.f);
}

// Фазы сцены на сервере (в снимок уходят номерами 0-3)
enum class Phase : std::uint8_t { Playing = 0, UpgradePick = 1, Victory = 2, GameOver = 3 };

class Instance
{
public:
    Instance() : systems_(reg_)
    {
        // Порядок систем ровно как в конструкторе GameScene. RenderSystem на сервере не нужен.
        systems_.push_module(std::make_shared<InputSystem>(reg_, data_));
        systems_.push_module(std::make_shared<EnemySystem>(reg_));
        systems_.push_module(std::make_shared<BossSystem>(reg_));
        systems_.push_module(std::make_shared<MoveSystem>(reg_));
        systems_.push_module(std::make_shared<CollisionSystem>(reg_, data_));
        systems_.push_module(std::make_shared<ContactDamageSystem>(reg_));
        systems_.push_module(std::make_shared<BoundsSystem>(reg_, ARENA_W, ARENA_H));
        systems_.push_module(std::make_shared<WallCollisionSystem>(reg_));
        systems_.push_module(std::make_shared<BeamSystem>(reg_));
        systems_.push_module(std::make_shared<WaveSystem>(reg_, data_, ARENA_W, ARENA_H));
        systems_.push_module(std::make_shared<BulletWallSystem>(reg_, data_));
        systems_.push_module(std::make_shared<ExplosionSystem>(reg_));
        systems_.push_module(std::make_shared<FlameSystem>(reg_));

        reset();                 // как GameScene::Init: сначала мир, потом Init и Start
        systems_.Init();
        systems_.Start();
    }

    ~Instance() { systems_.Stop(); }

    // Один тик сервера (1/TICK_RATE секунды) с вводом игрока
    void tick(const InputState& input)
    {
        switch (phase_)
        {
        case Phase::Playing:     tick_playing(input); break;
        case Phase::UpgradePick: tick_pick(input);    break;
        default:                 if (input.restart) reset(); break;   // победа или поражение
        }
    }

    // Снимок мира. Формат описан в Protocol.h.
    void write_world(std::uint32_t net_id, proto::Writer& w) const
    {
        w.u8(static_cast<std::uint8_t>(phase_));
        w.u8(static_cast<std::uint8_t>(optA_));
        w.u8(static_cast<std::uint8_t>(optB_));
        w.u16(static_cast<std::uint16_t>(std::clamp(data_.wave, 0, 65535)));
        w.u32(static_cast<std::uint32_t>(std::max(data_.score, 0)));

        // Игрок (если погиб, сущности нет: "нет игрока")
        const Entity pl = findPlayer(reg_);
        const Position* pp = pl.isNull() ? nullptr : reg_.tryGet<Position>(pl);
        const Health* hp = pl.isNull() ? nullptr : reg_.tryGet<Health>(pl);
        const Weapon* wp = pl.isNull() ? nullptr : reg_.tryGet<Weapon>(pl);
        if (pp && hp && wp)
        {
            w.u8(1);
            w.u32(net_id);
            w.i16(q16(pp->x));
            w.i16(q16(pp->y));
            w.u8(static_cast<std::uint8_t>(std::clamp(hp->hp, 0, 255)));
            w.u8(static_cast<std::uint8_t>(std::clamp(hp->max, 0, 255)));
            w.u8(static_cast<std::uint8_t>(wp->type));
        }
        else
        {
            w.u8(0);
        }

        // Враги
        const auto enemies = reg_.entitiesWith<Enemy, Position, Health>();
        w.u16(static_cast<std::uint16_t>(enemies.size()));
        for (Entity e : enemies)
        {
            const Position& p = reg_.get<Position>(e);
            const Health& h = reg_.get<Health>(e);
            w.u32((e.index & 0x00FFFFFFu) | ((e.generation & 0xFFu) << 24));
            w.u8(static_cast<std::uint8_t>(reg_.get<Enemy>(e).kind));
            w.i16(q16(p.x));
            w.i16(q16(p.y));
            w.u16(static_cast<std::uint16_t>(std::clamp(h.hp, 0, 65535)));
            w.u16(static_cast<std::uint16_t>(std::clamp(h.max, 0, 65535)));
        }

        // Пули (не больше 300)
        const auto bullets = reg_.entitiesWith<Bullet, Position>();
        const std::size_t nb = std::min<std::size_t>(bullets.size(), 300);
        w.u16(static_cast<std::uint16_t>(nb));
        for (std::size_t i = 0; i < nb; ++i)
        {
            const Position& p = reg_.get<Position>(bullets[i]);
            std::uint8_t flags = 0;
            if (reg_.get<Bullet>(bullets[i]).fromPlayer) flags |= 1;
            if (reg_.has<Explosive>(bullets[i]))         flags |= 2;
            w.u8(flags);
            w.i16(q16(p.x));
            w.i16(q16(p.y));
        }

        // Лучи (не больше 16)
        const auto beams = reg_.entitiesWith<Beam>();
        const std::size_t nbeams = std::min<std::size_t>(beams.size(), 16);
        w.u8(static_cast<std::uint8_t>(nbeams));
        for (std::size_t i = 0; i < nbeams; ++i)
        {
            const Beam& b = reg_.get<Beam>(beams[i]);
            w.i16(q16(b.x1)); w.i16(q16(b.y1));
            w.i16(q16(b.x2)); w.i16(q16(b.y2));
        }

        // Взрывы (не больше 32)
        const auto explosions = reg_.entitiesWith<Explosion>();
        const std::size_t nex = std::min<std::size_t>(explosions.size(), 32);
        w.u8(static_cast<std::uint8_t>(nex));
        for (std::size_t i = 0; i < nex; ++i)
        {
            const Explosion& x = reg_.get<Explosion>(explosions[i]);
            w.i16(q16(x.x));
            w.i16(q16(x.y));
            w.u16(static_cast<std::uint16_t>(std::clamp(x.radius, 0.f, 65535.f)));
            w.u8(t255(x.life, x.maxLife));
        }

        // Огонь (не больше 200 частиц)
        const auto flames = reg_.entitiesWith<Flame, Position>();
        const std::size_t nf = std::min<std::size_t>(flames.size(), 200);
        w.u16(static_cast<std::uint16_t>(nf));
        for (std::size_t i = 0; i < nf; ++i)
        {
            const Position& p = reg_.get<Position>(flames[i]);
            const Flame& f = reg_.get<Flame>(flames[i]);
            w.i16(q16(p.x));
            w.i16(q16(p.y));
            w.u8(t255(f.life, f.maxLife));
        }

        // Стены (меняются каждую волну)
        const auto walls = reg_.entitiesWith<Wall, Position>();
        const std::size_t nw = std::min<std::size_t>(walls.size(), 32);
        w.u8(static_cast<std::uint8_t>(nw));
        for (std::size_t i = 0; i < nw; ++i)
        {
            const Position& p = reg_.get<Position>(walls[i]);
            const Wall& wl = reg_.get<Wall>(walls[i]);
            w.i16(q16(p.x)); w.i16(q16(p.y));
            w.i16(q16(wl.w)); w.i16(q16(wl.h));
        }
    }

private:
    // ---- GameScene::Update, состояние Playing ----
    void tick_playing(const InputState& input)
    {
        systems_.SetInput(input);
        systems_.Update(DT);

        const GameState next = evaluateState(reg_);
        if (next != GameState::Playing)
        {
            phase_ = (next == GameState::Victory) ? Phase::Victory : Phase::GameOver;
            return;
        }

        bool anyEnemy = false;
        reg_.each<Enemy>([&](Entity, Enemy&) { anyEnemy = true; });

        // Волна только что зачищена (враги были, и вот их не стало) -> выбор бонуса
        if (hadEnemies_ && !anyEnemy && data_.wave > 0)
            enter_pick();
        hadEnemies_ = anyEnemy;
    }

    // ---- WaveClearScene: две разные карточки, ждём 1 или 2 ----
    void enter_pick()
    {
        optA_ = random_upgrade();
        do { optB_ = random_upgrade(); } while (optB_ == optA_);
        pick_delay_ = 0.2f;              // защита от "долетевшего" нажатия
        phase_ = Phase::UpgradePick;
    }

    void tick_pick(const InputState& input)
    {
        pick_delay_ -= DT;
        if (pick_delay_ > 0.f) return;

        const int pick = input.num1 ? 1 : input.num2 ? 2 : 0;
        if (pick == 0) return;

        const Entity player = findPlayer(reg_);
        if (!player.isNull())
            applyUpgrade(reg_, player, pick == 1 ? optA_ : optB_);
        phase_ = Phase::Playing;         // мир не пересоздаётся: игра продолжается
    }

    UpgradeKind random_upgrade()
    {
        static const UpgradeKind pool[] = {
            UpgradeKind::MaxHp, UpgradeKind::Heal, UpgradeKind::FireRate,
            UpgradeKind::Speed, UpgradeKind::Shotgun, UpgradeKind::Laser,
            UpgradeKind::Bazooka
        };
        return pool[std::uniform_int_distribution<int>(0, (int)(sizeof(pool) / sizeof(pool[0])) - 1)(rng_)];
    }

    // ---- GameScene::Reset ----
    void reset()
    {
        resetWorld(reg_);
        data_.score = 0;
        data_.wave = 0;
        hadEnemies_ = false;
        phase_ = Phase::Playing;
    }

    Registry reg_;
    GameData data_;
    ModuleRegistry systems_;
    std::mt19937 rng_{ std::random_device{}() };
    Phase phase_ = Phase::Playing;
    UpgradeKind optA_ = UpgradeKind::MaxHp, optB_ = UpgradeKind::Heal;
    float pick_delay_ = 0.f;
    bool hadEnemies_ = false;
};