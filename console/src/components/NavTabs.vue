<script setup>
import { useRoute } from "vue-router";

const route = useRoute();

const navItems = [
  { id: "timing", icon: "⏱️", label: "Timing", path: "/" },
  { id: "history", icon: "📋", label: "History", path: "/history" },
  { id: "settings", icon: "⚙️", label: "Settings", path: "/settings" },
];
</script>

<template>
  <nav class="nav-tabs">
    <router-link v-for="item in navItems" :key="item.id" :to="item.path" class="nav-tab" :class="{ active: route.path === item.path }">
      <span class="nav-icon" aria-hidden="true">{{ item.icon }}</span>
      <span>{{ item.label }}</span>
    </router-link>
  </nav>
</template>

<style scoped>
.nav-tabs {
  display: flex;
  gap: 0.25rem;
  background: transparent;
  padding: 0.25rem;
  border-radius: 12px;
  flex: 1;
  justify-content: center;
  flex-wrap: wrap;
}

.nav-tab {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  padding: 0.5rem 1rem;
  border-radius: 8px;
  color: var(--text-secondary);
  text-decoration: none;
  font-size: 1rem;
  font-weight: 500;
  transition: background-color 0.2s ease, color 0.2s ease, box-shadow 0.2s ease;
  border: none;
  background: none;
  white-space: nowrap;
}

.nav-icon {
  flex-shrink: 0;
  line-height: 1;
}

.nav-tab:hover {
  background: var(--bg-hover);
  color: var(--text-primary);
}

.nav-tab.active {
  background: var(--bg-tab-active, var(--bg-card));
  color: var(--text-primary);
  font-weight: 700;
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.16), 0 1px 3px rgba(0, 0, 0, 0.1);
}

@media (max-width: 1024px) {
  .nav-tabs {
    display: grid;
    grid-template-columns: repeat(3, minmax(0, 1fr));
    grid-column: 1 / -1;
    grid-row: 2;
    gap: 0.625rem;
  }

  .nav-tab {
    justify-content: center;
    padding: 0.45rem 0.6rem;
  }
}

@media (max-width: 380px) {
  .nav-tab { flex-direction: column; gap: 0.25rem; }
}
</style>
