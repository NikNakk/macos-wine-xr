// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// macos-wine-xr's options on a game's Compatibility page in Steam (docs/steam-play.md,
// The Compatibility page). Served to Steam's interface as /mwxr/options.js by the compat
// gate library (steam_ui_patch.c), whose patch of sp.js puts an empty element under the
// tool dropdown and calls globalThis.mwxrOptions(element, answer) for it.
//
// The options are not stored anywhere of their own: each one is an MWXR_NAME=value word
// in the game's Launch Options (Properties, General), which the Steam Play tool already
// exports for the launch. The page reads them with SteamClient.Apps.RegisterForAppDetails
// and writes them with SteamClient.Apps.SetAppLaunchOptions, as the General page does, so
// the two pages always show the same thing. Only the words an option owns are changed;
// game arguments, %command% and other MWXR_ words stay as they are.
(() => {
  "use strict";
  const OUR_TOOL = "macos-wine-xr-proton";

  // --- Launch Options words -------------------------------------------------------------
  const words = (s) => (s || "").split(/\s+/).filter(Boolean);
  const value = (s, name) => {
    const w = words(s).find((x) => x.startsWith(name + "="));
    return w === undefined ? undefined : w.slice(name.length + 1);
  };
  // values: {NAME: "value" | undefined}; undefined removes the word. New words go first.
  const withValues = (s, values) => {
    let rest = s || "";
    for (const name of Object.keys(values))
      rest = rest.replace(new RegExp("(^|\\s+)" + name + "=\\S*", "g"), "");
    rest = rest.replace(/^\s+/, "");
    const add = Object.entries(values)
      .filter(([, v]) => v !== undefined && v !== "")
      .map(([k, v]) => k + "=" + v);
    return [...add, rest].filter(Boolean).join(" ");
  };

  // --- The options ----------------------------------------------------------------------
  // Each: label, description, choices [{value, label}], read(launch options) -> value,
  // write(value) -> {NAME: value | undefined}.
  const OPTIONS = [
    {
      id: "uevr",
      label: "UEVR",
      description: "Unreal Engine games in VR: injects UEVR once the game's window is up. Its menu opens with Insert.",
      choices: [
        { value: "", label: "Off" },
        { value: "openxr", label: "On (OpenXR)" },
        { value: "openvr", label: "On (OpenVR, through xrizer)" },
      ],
      read: (s) => (value(s, "MWXR_UEVR") === "1" ? value(s, "MWXR_UEVR_RUNTIME") || "openxr" : ""),
      write: (v) => ({
        MWXR_UEVR: v ? "1" : undefined,
        MWXR_UEVR_RUNTIME: v === "openvr" ? "openvr" : undefined,
      }),
    },
    {
      id: "foveation",
      label: "Foveated rendering",
      description: "Renders the edges of each eye at a lower rate: fixed at the centre, or following your gaze on a headset with eye tracking.",
      choices: [
        { value: "", label: "Off" },
        { value: "fixed", label: "Fixed" },
        { value: "gaze", label: "Gaze (eye tracking)" },
      ],
      read: (s) => {
        const v = value(s, "MWXR_FOVEATION");
        return v === undefined || v === "off" ? "" : v;
      },
      write: (v) => ({ MWXR_FOVEATION: v || undefined }),
    },
  ];

  // --- Steam's app details, one subscription per game -------------------------------------
  const games = new Map(); // appid -> {details, elements: Set}
  function game(appid) {
    let g = games.get(appid);
    if (g) return g;
    g = { details: null, elements: new Set() };
    games.set(appid, g);
    try {
      SteamClient.Apps.RegisterForAppDetails(appid, (details) => {
        g.details = details;
        for (const el of [...g.elements]) {
          if (el.isConnected) draw(el);
          else g.elements.delete(el);
        }
      });
    } catch (e) {
      console.error("mwxr: RegisterForAppDetails", e);
    }
    return g;
  }

  function ours(answer) {
    const selected = answer?.selected_tool_identifier;
    return selected === OUR_TOOL || (!selected && answer?.default_tool_identifier === OUR_TOOL);
  }

  function save(appid, values) {
    const g = game(appid);
    const before = g.details?.strLaunchOptions ?? "";
    const after = withValues(before, values);
    if (after === before) return;
    console.log("mwxr: Launch Options for", appid, JSON.stringify(before), "->", JSON.stringify(after));
    SteamClient.Apps.SetAppLaunchOptions(appid, after);
  }

  function draw(el) {
    const answer = el.mwxrAnswer, appid = answer?.mwxr_appid;
    const doc = el.ownerDocument;
    const g = appid ? game(appid) : null;
    el.replaceChildren();
    if (!g || !ours(answer)) return;
    if (!g.details) {
      el.append(text(doc, "div", "Loading macos-wine-xr options…", "mwxr-note"));
      return;
    }
    const launch = g.details.strLaunchOptions ?? "";
    const box = doc.createElement("div");
    box.style.cssText = "margin-top:16px;padding-top:12px;border-top:1px solid rgba(255,255,255,.1)";
    box.append(text(doc, "div", "macos-wine-xr", null, "font-weight:600;color:#dcdedf;margin-bottom:8px"));
    for (const option of OPTIONS) box.append(row(doc, appid, option, launch));
    const note = text(doc, "div",
      "Saved in this game's Launch Options (General), as MWXR_ words; a change there shows here.",
      null, "margin-top:10px;font-size:12px;color:#8b929a");
    box.append(note);
    if (OPTIONS[0].read(launch)) {
      const button = doc.createElement("button");
      button.className = "DialogButton";
      button.textContent = "Open UEVR settings folder";
      button.style.cssText = "display:inline-block;width:auto;min-width:0;padding:0 12px;margin-top:10px";
      button.onclick = (ev) => {
        ev.stopPropagation();
        fetch("/mwxr/uevr-folder/" + appid + ".json?" + Date.now())
          .then((r) => r.json())
          .then((r) => { if (!r.ok) button.textContent = "UEVR settings folder: " + (r.error || "not found"); })
          .catch(() => {});
      };
      box.append(button);
    }
    el.append(box);
  }

  function row(doc, appid, option, launch) {
    const current = option.read(launch);
    // One column: the options sit in the tool dropdown's description, under its label.
    const wrap = doc.createElement("label");
    wrap.style.cssText = "display:flex;flex-direction:column;gap:6px;margin:12px 0";
    const top = doc.createElement("div");
    top.style.cssText = "display:flex;flex-direction:column;gap:6px";
    top.append(text(doc, "span", option.label, null, "color:#dcdedf"));
    const select = doc.createElement("select");
    select.style.cssText = "width:100%;box-sizing:border-box;background:#32353c;color:#dcdedf;border:1px solid #4a4e57;border-radius:2px;padding:6px 8px";
    const choices = [...option.choices];
    // A value the page has no choice for (gaze:0.6, say) is kept and shown as it is.
    if (!choices.some((c) => c.value === current)) choices.push({ value: current, label: "Custom: " + current });
    for (const c of choices) {
      const o = doc.createElement("option");
      o.value = c.value;
      o.textContent = c.label;
      o.selected = c.value === current;
      select.append(o);
    }
    select.onchange = () => save(appid, option.write(select.value));
    select.onclick = (ev) => ev.stopPropagation();
    top.append(select);
    wrap.append(top, text(doc, "div", option.description, null, "font-size:12px;color:#8b929a"));
    return wrap;
  }

  function text(doc, tag, content, className, style) {
    const e = doc.createElement(tag);
    e.textContent = content;
    if (className) e.className = className;
    if (style) e.style.cssText = style;
    return e;
  }

  globalThis.mwxrOptions = (el, answer) => {
    if (!el) return;
    el.mwxrAnswer = answer;
    if (answer?.mwxr_appid) game(answer.mwxr_appid).elements.add(el);
    draw(el);
  };
  // For tests and the console.
  globalThis.mwxrOptions.withValues = withValues;
  globalThis.mwxrOptions.options = OPTIONS;
})();
