// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Mario Baril
// The pages' shared behaviour: the language (kept in this browser), and on the reading pages the chapter list,
// built from the headings of the language shown, with the chapter being read marked.
(function () {
  "use strict";
  const root = document.documentElement, en = document.getElementById("en"), pt = document.getElementById("pt");
  let observer = null;
  function chapters() {
    const lists = document.querySelectorAll("[data-toc]");
    if (!lists.length) return;
    const lang = root.lang === "pt-BR" ? "pt" : "en";
    const heads = [...document.querySelectorAll('.article [data-l="' + lang + '"] h2, .article > h2[data-l="' + lang + '"]')];
    heads.forEach((h, i) => { if (!h.id) h.id = lang + "-" + (i + 1); });
    for (const list of lists) {
      list.innerHTML = "";
      for (const h of heads) {
        const a = document.createElement("a");
        a.href = "#" + h.id;
        const small = h.querySelector("small");
        a.textContent = (small ? small.textContent.trim() + "  " : "") + [...h.childNodes].filter(n => n.nodeType === 3).map(n => n.textContent).join("").trim();
        list.append(a);
      }
    }
    if (observer) observer.disconnect();
    if (!("IntersectionObserver" in window)) return;
    observer = new IntersectionObserver((entries) => {
      for (const e of entries) if (e.isIntersecting) {
        for (const a of document.querySelectorAll("[data-toc] a")) a.classList.toggle("on", a.getAttribute("href") === "#" + e.target.id);
      }
    }, { rootMargin: "-20% 0px -70% 0px" });
    heads.forEach((h) => observer.observe(h));
  }
  function set(l) {
    root.lang = l === "pt" ? "pt-BR" : "en";
    if (en && pt) {
      en.classList.toggle("on", l !== "pt"); pt.classList.toggle("on", l === "pt");
      en.setAttribute("aria-pressed", String(l !== "pt")); pt.setAttribute("aria-pressed", String(l === "pt"));
    }
    try { localStorage.setItem("faisca.lang", l); } catch (e) { /* private mode: the choice lasts this page */ }
    chapters();
    document.dispatchEvent(new CustomEvent("faisca-lang", { detail: l }));
  }
  let l = "en";
  try { l = localStorage.getItem("faisca.lang") || ((navigator.language || "").toLowerCase().startsWith("pt") ? "pt" : "en"); } catch (e) { /* default */ }
  if (en) en.addEventListener("click", () => set("en"));
  if (pt) pt.addEventListener("click", () => set("pt"));
  set(l);
})();
