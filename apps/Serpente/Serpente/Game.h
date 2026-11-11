#pragma once
// The game without any UI: a grid, the snake, the food. Portable (tested on the PC: tests/game_test.cpp).
#include <deque>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace Serpente
{
    struct Cell { int x, y; };
    inline bool operator==(Cell a, Cell b) { return a.x == b.x && a.y == b.y; }

    enum Dir { Up, Right, Down, Left };

    class Game
    {
    public:
        Game(int cols, int rows, uint32_t seed)
            : cols_(cols), rows_(rows), rng_(seed ? seed : 1), dir_(Right), grow_(0), score_(0), over_(false), full_(false)
        {
            food_.x = food_.y = 0;
            Reset();
        }

        void Reset()
        {
            body_.clear();
            int y = rows_ / 2;
            for (int i = 0; i < 4; i++) body_.push_front(Cell{ 2 + i, y });  // front = head, heading right
            dir_ = Right;
            pending_.clear();
            grow_ = 0;
            score_ = 0;
            over_ = false;
            full_ = false;
            PlaceFood();
        }

        // A swipe. Up to two turns wait for the next steps (a quick "down, left" makes a U-turn without dying);
        // a reverse or a repeat of the direction it already goes is ignored.
        void Turn(Dir d)
        {
            Dir last = pending_.empty() ? dir_ : pending_.back();
            if (d == last || d == Opposite(last) || pending_.size() >= 2) return;
            pending_.push_back(d);
        }

        // One step. Returns true when the food was eaten in this step.
        bool Step()
        {
            if (over_) return false;
            if (!pending_.empty()) { dir_ = pending_.front(); pending_.erase(pending_.begin()); }
            Cell h = body_.front();
            switch (dir_)
            {
            case Up: h.y--; break;
            case Down: h.y++; break;
            case Left: h.x--; break;
            case Right: h.x++; break;
            }
            if (h.x < 0 || h.y < 0 || h.x >= cols_ || h.y >= rows_) { over_ = true; return false; }
            // the tail moves away in this same step (unless it grows), so the head may take its cell
            bool tailLeaves = grow_ == 0;
            for (size_t i = 0; i < body_.size(); i++)
            {
                if (tailLeaves && i == body_.size() - 1) break;
                if (body_[i] == h) { over_ = true; return false; }
            }
            body_.push_front(h);
            if (grow_ > 0) grow_--; else body_.pop_back();
            bool ate = h == food_;
            if (ate)
            {
                score_++;
                grow_ += 2;
                PlaceFood();
            }
            return ate;
        }

        // milliseconds per step: faster as it eats, down to 70
        int Interval() const { int ms = 170 - score_ * 4; return ms < 70 ? 70 : ms; }

        const std::deque<Cell>& Body() const { return body_; }
        Cell Food() const { return food_; }
        int Score() const { return score_; }
        bool Over() const { return over_; }
        int Cols() const { return cols_; }
        int Rows() const { return rows_; }
        bool Full() const { return full_; }

    private:
        static Dir Opposite(Dir d) { return static_cast<Dir>((d + 2) % 4); }

        uint32_t Next() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return rng_; }

        void PlaceFood()
        {
            std::vector<char> used(cols_ * rows_, 0);
            for (auto& c : body_) used[c.y * cols_ + c.x] = 1;
            int freeCells = 0;
            for (char u : used) if (!u) freeCells++;
            if (freeCells == 0) { full_ = true; over_ = true; food_ = Cell{ -1, -1 }; return; }
            int k = static_cast<int>(Next() % static_cast<uint32_t>(freeCells));
            for (int i = 0; i < cols_ * rows_; i++)
                if (!used[i] && k-- == 0) { food_ = Cell{ i % cols_, i / cols_ }; return; }
        }

        int cols_, rows_;
        uint32_t rng_;
        std::deque<Cell> body_;
        std::vector<Dir> pending_;
        Dir dir_;
        Cell food_;
        int grow_;
        int score_;
        bool over_;
        bool full_;
    };
}
