<script setup>
import { useRoute } from "vue-router";

const route = useRoute();

const navItems = [
  { id: "timing", label: "Timing", path: "/" },
  { id: "history", label: "History", path: "/history" },
  { id: "settings", label: "Settings", path: "/settings" },
];
</script>

<template>
  <nav class="nav-tabs">
    <router-link v-for="item in navItems" :key="item.id" :to="item.path" class="nav-tab" :class="{ active: route.path === item.path }">
      {{ item.label }}
    </router-link>
  </nav>
</template>

<style scoped>
.nav-tabs {
  display: flex;
  gap: 0.375rem;
  flex: 1;
  justify-content: center;
  flex-wrap: wrap;
}

.nav-tab {
  display: flex;
  align-items: center;
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
