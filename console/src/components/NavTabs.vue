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
  gap: 1rem;
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
  color: #b0b4be;
  text-decoration: none;
  font-size: 0.875rem;
  font-weight: 500;
  transition: background-color 0.15s ease, color 0.15s ease;
  background: rgba(255, 255, 255, 0.05);
  white-space: nowrap;
}

.nav-icon {
  flex-shrink: 0;
  line-height: 1;
}

.nav-tab:hover {
  background: rgba(255, 255, 255, 0.1);
  color: #e2e4e9;
}

.nav-tab.active {
  background: rgba(255, 255, 255, 0.12);
  color: #e2e4e9;
}

@media (max-width: 640px) {
  .nav-tabs {
    gap: 0.625rem;
    order: 3;
    flex: 0 0 100%;
    justify-content: center;
  }

  .nav-tab {
    flex: 1;
    justify-content: center;
    padding: 0.45rem 0.6rem;
    font-size: 0.8rem;
  }
}
</style>
