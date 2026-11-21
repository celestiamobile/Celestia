// scriptrunner.h
//
// Copyright (C) 2026, the Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include "script.h"

#include <memory>
#include <vector>

namespace celestia::scripts
{

class ScriptRunner
{
public:
    ScriptRunner() = default;
    ScriptRunner(const ScriptRunner &) = delete;
    ScriptRunner &operator=(const ScriptRunner &) = delete;
    ~ScriptRunner();

    enum class State
    {
        Running,
        Paused,
    };

    void add(std::unique_ptr<IScript>, State state = State::Running);
    void clear();
    bool empty() const
    {
        return m_scripts.empty();
    }
    bool isCanceling() const
    {
        return m_canceling;
    }
    void setPaused(bool paused);
    bool allPaused() const;
    bool wantsKeyboard() const;
    bool wantsExclusiveInput() const;

    void tick(double dt);
    bool charEntered(const char *);
    bool handleKeyEvent(const char *);
    bool handleMouseButtonEvent(float x, float y, int button, bool down);

private:
    struct Entry
    {
        Entry(ScriptRunner &, std::unique_ptr<IScript>, State);
        ~Entry();

        ScriptRunner &owner;
        std::unique_ptr<IScript> script;
        State state;
        bool canceled{ false };
    };

    template<typename F>
    bool dispatch(F &&, bool exclusiveCharacters = false);
    bool contains(const std::shared_ptr<Entry> &) const;
    std::shared_ptr<Entry> exclusiveScript();

    std::vector<std::shared_ptr<Entry>> m_scripts;
    std::weak_ptr<Entry> m_exclusiveScript;
    bool m_dispatching{ false };
    bool m_canceling{ false };
};

} // namespace celestia::scripts
