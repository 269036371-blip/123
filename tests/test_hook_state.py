#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
KeyMacro 钩子状态机回归测试
Author: 霭阳霁月 (ayjy)

用途
----
把 src/main.cpp 里 LowLevelKeyboardProc 的"吞 / 透传"决策逻辑逐行移植到
Python，用事件序列驱动，断言两件事：

  1. 系统侧收到的 down / up 必须严格配对
     —— 出现孤立 down 会让系统认为键一直按着；
        出现孤立 up 说明我们的配对标记错了。
  2. 松开物理键后 physicalDown 必须为 False
     —— 否则连发停不下来（P0-B 的"松手后还连发一秒"）。

这不是在跑真实 Windows 代码（那需要 Windows + 真键盘），而是对
"决策逻辑"的模型化回归测试。C++ 侧改动时请同步更新本文件。

运行
----
    python3 tests/test_hook_state.py
    python3 -m pytest tests/test_hook_state.py -v   # 也支持 pytest

退出码 0 = 全部通过，1 = 有失败。
"""

import sys

# ---- 常量（与 main.cpp 一致）----
VK_F8 = 0x77
VK_F12 = 0x7B
CTRL, ALT, LWIN, RWIN = 0x11, 0x12, 0x5B, 0x5C


class KeyState:
    """对应 C++ 的 struct KeyState，只保留与本测试相关的字段。"""

    def __init__(self, vk):
        self.vk = vk
        self.physicalDown = False   # 物理键是否按住（驱动连发）
        self.isDown = False         # 我方已发 DOWN 未配 UP
        self.swallowedDown = False  # down/up 配对标记


class HookModel:
    """LowLevelKeyboardProc 的决策逻辑移植。

    返回值语义与 C++ 一致：
        True  = return 1（吞掉，不传给系统）
        False = CallNextHookEx（透传给系统）
    """

    def __init__(self, whitelist=(0x31,)):  # 0x31 = '1'
        self.keys = {vk: KeyState(vk) for vk in whitelist}
        self.enabled = True
        self.mods = set()          # 当前按住的修饰键
        self.f8Down = False
        self.f12Down = False
        self.to_system = []        # 透传给系统的事件：(key, 'down'/'up')
        self.quit = False

    # ---- 辅助 ----
    def modifier_held(self):
        return bool(self.mods)

    def ctrl_alt(self):
        return CTRL in self.mods and ALT in self.mods \
            and LWIN not in self.mods and RWIN not in self.mods

    def _emit(self, key, kind):
        self.to_system.append((key, kind))

    # ---- 事件入口 ----
    def key(self, vk, kind):
        """kind: 'down' / 'up'；返回 True 表示被吞。"""
        if kind == 'down':
            self.mods.add(vk) if vk in (CTRL, ALT, LWIN, RWIN) else None
        else:
            self.mods.discard(vk)

        # ---- F8：切换启用（含 latch）----
        if vk == VK_F8:
            if kind == 'up':
                self.f8Down = False
                return True
            if self.f8Down:
                return True
            self.f8Down = True
            if not self.modifier_held():
                self.enabled = not self.enabled
                if not self.enabled:
                    # 禁用：清 physicalDown 停止连发。
                    # 注意不清 swallowedDown —— 这是孤立 up 的关键。
                    for ks in self.keys.values():
                        ks.physicalDown = False
            return True

        # ---- F12：Ctrl+Alt+F12 退出 ----
        if vk == VK_F12:
            # up 先处理且无条件清 latch（FIX-1）。
            # 不能先判 ctrl_alt()：组合键 down 触发退出后，用户很可能先松
            # Ctrl/Alt 再松 F12，此时 up 到达时组合键已不成立，若走透传
            # 分支就会漏掉清 latch —— 残留的 True 会让下一次组合键的 down
            # 被 "if self.f12Down: return True" 吞掉却不触发退出，
            # 第一次按键白按。
            if kind == 'up':
                was_swallowed = self.f12Down
                self.f12Down = False
                if was_swallowed:
                    return True              # 吞，与当初吞的 down 配对
                self._emit(vk, 'up')         # 没吞过 down → 透传
                return False
            if not self.ctrl_alt():
                # 普通 F12 down：完全透传，不设 latch，自动重复不受影响
                self._emit(vk, 'down')
                return False
            if self.f12Down:
                return True                  # 自动重复
            self.f12Down = True
            self.quit = True
            return True

        # ---- 白名单键 ----
        if vk not in self.keys:
            return False
        ks = self.keys[vk]

        if not self.enabled:
            return False

        if kind == 'up':
            # 无条件停连发
            ks.physicalDown = False
            was_ours = ks.swallowedDown
            ks.swallowedDown = False
            if was_ours:
                return True          # 吞 up，与当初吞的 down 配对
            self._emit(vk, 'up')     # 没吞过 down → 透传，避免孤立 up
            return False

        # ---- down ----
        if self.modifier_held():
            self._emit(vk, 'down')   # 修饰键组合透传，不连发
            return False

        if ks.physicalDown:
            return True              # 系统自动重复

        ks.physicalDown = True
        ks.isDown = True
        ks.swallowedDown = True      # 记下：down 被吞
        return True                  # 吞掉原始按下，由我们发首发

    # ---- 换键位（对应 IDM_RELOAD）----
    def reload(self):
        """对应 ReleaseAllKeys() + InitKeys()。

        FIX-2 / FIX-3：这两处绝不能清 swallowedDown。若清掉而用户仍按着
        键，稍后到达的物理 up 会被误判为"没吞过 down"而透传，
        系统侧收到孤立 up。
        """
        for ks in self.keys.values():
            if ks.isDown:
                ks.isDown = False            # 补发注入 up
            ks.physicalDown = False
            # ks.swallowedDown 保持不变 —— 这正是本用例要验证的

    # ---- 断言辅助 ----
    def release(self, vk):
        """模拟调度线程在 physicalDown 变 False 后补发注入 up。"""
        ks = self.keys[vk]
        if ks.isDown:
            ks.isDown = False


def check_pairing(events):
    """检查 down/up 是否配对。返回 (是否配对, 描述)。"""
    stack = []
    for key, kind in events:
        if kind == 'down':
            stack.append(key)
        else:
            if not stack or stack[-1] != key:
                return False, f"孤立 up: {key:#x}（此前 stack={stack}）"
            stack.pop()
    if stack:
        return False, f"孤立 down（未收到 up）: {[hex(k) for k in stack]}"
    return True, "配对完整"


# ============================================================
# 测试用例
# ============================================================
def run_case(name, fn):
    passed, msg = fn()
    status = "PASS" if passed else "FAIL"
    print(f"  [{status}] {name}")
    if msg:
        print(f"         {msg}")
    return passed


def case_plain():
    """按住 1 → 松开 1：系统侧应完全不知道，连发立即停。"""
    m = HookModel()
    m.key(0x31, 'down')
    assert m.keys[0x31].physicalDown is True, "按下后应开始连发"
    m.key(0x31, 'up')
    ok, d = check_pairing(m.to_system)
    stopped = not m.keys[0x31].physicalDown
    return ok and stopped, f"系统侧: {d}; 连发已停: {stopped}"


def case_modifier_hold():
    """Ctrl + 1 → 松开：不连发，down/up 都透传且配对。"""
    m = HookModel()
    m.key(CTRL, 'down')
    m.key(0x31, 'down')
    no_fire = not m.keys[0x31].physicalDown
    m.key(0x31, 'up')
    m.key(CTRL, 'up')
    ok, d = check_pairing(m.to_system)
    return ok and no_fire, f"系统侧: {d}; 未连发: {no_fire}"


def case_modifier_while_held():
    """P0-B 核心：按住 1 → 按 Ctrl → 松开 1，连发必须立即停。"""
    m = HookModel()
    m.key(0x31, 'down')
    assert m.keys[0x31].physicalDown is True
    m.key(CTRL, 'down')
    m.key(0x31, 'up')
    stopped = not m.keys[0x31].physicalDown
    m.key(CTRL, 'up')
    ok, d = check_pairing(m.to_system)
    return ok and stopped, f"系统侧: {d}; 松手即停: {stopped}"


def case_disable_while_held():
    """按住 1 → F8 禁用 → 松开 1。

    这是 swallowedDown 存在的理由：禁用时清了 physicalDown，
    若配对信息也复用它，这里会透传出一个孤立 up。
    """
    m = HookModel()
    m.key(0x31, 'down')
    m.key(VK_F8, 'down')
    m.key(VK_F8, 'up')
    assert m.enabled is False, "F8 应已禁用"
    m.key(0x31, 'up')
    ok, d = check_pairing(m.to_system)
    stopped = not m.keys[0x31].physicalDown
    return ok and stopped, f"系统侧: {d}; 连发已停: {stopped}"


def case_plain_f12():
    """普通 F12 down/up：完全透传，且自动重复不被 latch 吞。

    注意：自动重复会产生多个 down 却只有一个 up，这是 Windows 的真实
    行为，不属于"不配对"。所以配对检查只用单次 down+up，
    自动重复单独用计数验证（2 个 down 都应透传，而不是被 latch 吞掉）。
    """
    m = HookModel()
    m.key(VK_F12, 'down')
    m.key(VK_F12, 'up')
    ok, d = check_pairing(m.to_system)

    m2 = HookModel()
    m2.key(VK_F12, 'down')
    m2.key(VK_F12, 'down')   # 系统自动重复
    m2.key(VK_F12, 'up')
    downs = sum(1 for k, t in m2.to_system if k == VK_F12 and t == 'down')
    ups = sum(1 for k, t in m2.to_system if k == VK_F12 and t == 'up')
    ok2 = (downs == 2 and ups == 1)   # 重复没被吞
    return ok and ok2, f"单按: {d}; 自动重复 down={downs} up={ups}（期望 2/1）"


def case_ctrl_alt_f12():
    """Ctrl+Alt+F12：全部吞掉，系统侧收不到任何 F12 事件。"""
    m = HookModel()
    m.key(CTRL, 'down')
    m.key(ALT, 'down')
    m.key(VK_F12, 'down')
    m.key(VK_F12, 'up')
    m.key(ALT, 'up')
    m.key(CTRL, 'up')
    no_f12 = all(k != VK_F12 for k, _ in m.to_system)
    return m.quit and no_f12, f"已触发退出: {m.quit}; 系统侧无 F12: {no_f12}"


def case_f12_latch_residue():
    """FIX-1：组合键触发退出后先松 Ctrl/Alt，latch 不得残留。

    序列：Ctrl+Alt+F12 down（触发退出）→ 松 Ctrl → 松 Alt → 松 F12
          → 再按 Ctrl+Alt+F12 down，必须能再次触发退出。
    旧逻辑在"松 F12"时因组合键已不成立而走透传分支、漏清 latch，
    第二次组合键会被 latch 吞掉 —— 表现为"第一次按键白按"。
    """
    m = HookModel()
    m.key(CTRL, 'down')
    m.key(ALT, 'down')
    m.key(VK_F12, 'down')
    first = m.quit
    m.key(CTRL, 'up')        # 手快：先松了 Ctrl/Alt
    m.key(ALT, 'up')
    m.key(VK_F12, 'up')      # 此时组合键已不成立
    latch_cleared = (m.f12Down is False)

    m.quit = False           # 重置以便观察第二次是否触发
    m.key(CTRL, 'down')
    m.key(ALT, 'down')
    m.key(VK_F12, 'down')
    second = m.quit

    ok, d = check_pairing(m.to_system)
    return first and latch_cleared and second and ok, \
        f"首次退出: {first}; latch 已清: {latch_cleared}; " \
        f"二次可触发: {second}; 系统侧: {d}"


def case_reload_while_held():
    """FIX-2 / FIX-3：按住键时换键位（reload），松开不得产生孤立 up。

    若 ReleaseAllKeys / InitKeys 清了 swallowedDown，松开时物理 up 会被
    误判为"没吞过 down"而透传给系统 —— 系统侧收到一个没有配对的 up。
    """
    m = HookModel()
    m.key(0x31, 'down')
    assert m.keys[0x31].physicalDown is True
    m.reload()                               # 用户按着键时点了重新加载
    m.key(0x31, 'up')
    ok, d = check_pairing(m.to_system)
    stopped = not m.keys[0x31].physicalDown
    return ok and stopped, f"系统侧: {d}; 连发已停: {stopped}"


def main():
    print("KeyMacro 钩子状态机回归测试")
    print("=" * 62)
    results = [
        run_case("按住 1 → 松开 1（基础连发与停止）", case_plain),
        run_case("Ctrl + 1 → 松开（修饰键不连发）", case_modifier_hold),
        run_case("按住 1 → 按 Ctrl → 松开 1（P0-B）", case_modifier_while_held),
        run_case("按住 1 → F8 禁用 → 松开 1（配对标记）", case_disable_while_held),
        run_case("普通 F12 down/up（P0-A 透传）", case_plain_f12),
        run_case("Ctrl+Alt+F12 退出（全部吞掉）", case_ctrl_alt_f12),
        run_case("组合键后先松 Ctrl/Alt（FIX-1 latch 残留）",
                 case_f12_latch_residue),
        run_case("按住键时 reload（FIX-2/FIX-3 配对标记）",
                 case_reload_while_held),
    ]
    print("=" * 62)
    n_pass = sum(1 for r in results if r)
    print(f"通过 {n_pass}/{len(results)}")
    if n_pass != len(results):
        print("存在失败用例，请检查钩子状态机改动。")
        return 1
    print("全部通过。")
    return 0


if __name__ == '__main__':
    sys.exit(main())
