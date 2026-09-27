import { createRouter, createWebHashHistory } from "vue-router";
import TimingView from "../views/TimingView.vue";
import HistoryView from "../views/HistoryView.vue";
import SettingsView from "../views/SettingsView.vue";

const routes = [
  { path: "/", name: "timing", component: TimingView },
  { path: "/history", name: "history", component: HistoryView },
  { path: "/settings", name: "settings", component: SettingsView },
  { path: "/:pathMatch(.*)*", redirect: "/" },
];

// Hash history works from file://, from GitHub Pages and from any static host
// without rewrite rules, so every build mode uses it.
const router = createRouter({
  history: createWebHashHistory(),
  routes,
});

export default router;
