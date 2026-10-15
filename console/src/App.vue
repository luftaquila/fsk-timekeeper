<script setup>
import { onMounted, ref } from "vue";
import ThemeToggle from "./components/ThemeToggle.vue";
import NavTabs from "./components/NavTabs.vue";
import QualityAlert from "./components/QualityAlert.vue";
import MasterConnectionLight from "./components/MasterConnectionLight.vue";
import { useTimingStore } from "./stores/timing";
import { useHistoryStore } from "./stores/history";
import { useDeviceStore } from "./stores/device";
import * as eventLog from "./lib/eventLog";
import * as ppsLog from "./lib/ppsLog";
import * as deviceLog from "./lib/deviceLog";

const timing = useTimingStore();
const history = useHistoryStore();
const device = useDeviceStore();
const ready = ref(false);

onMounted(async () => {
  // Restore the open run first so its evidence window is loaded and protected.
  const minSeq = timing.restore();
  const { durable, error } = await eventLog.init({ minSeq });
  eventLog.protect(minSeq);
  device.durable = durable;
  device.durableError = error?.message || null;
  if (error) await deviceLog.addEntry({ kind: "console", code: "storage_failure", at: Date.now(), text: error.message || String(error) });
  device.requestPersistence();
  await history.init();
  await device.loadLog();
  await timing.reevaluate();
  ready.value = true;
  eventLog.pruneStore().catch(() => {});
  ppsLog.pruneEdges();
  deviceLog.pruneEntries();
});
</script>

<template>
  <div class="app-container">
    <header class="header">
      <div class="header-content">
        <router-link to="/" class="logo">
          <span class="logo-icon">⏱️</span>
          <h1>FSK Timekeeper Console</h1>
        </router-link>
        <NavTabs />
        <div class="header-actions">
          <MasterConnectionLight class="link-dot" />
          <ThemeToggle />
        </div>
      </div>
    </header>

    <div v-if="!device.durable" class="banner" role="alert">
      <div><strong>Browser storage unavailable</strong><p>Records will be lost on reload</p></div>
    </div>
    <div v-else-if="device.storageWarning" class="banner warn" role="alert">Browser storage is not protected from automatic cleanup</div>
    <div v-for="alarm in device.alarms" :key="alarm.key" class="banner" role="alert">
      <span>{{ alarm.text }}</span>
      <button class="banner-close" type="button" aria-label="Dismiss" @click="device.dismissAlarm(alarm.key)">×</button>
    </div>
    <div v-if="device.connected && device.identity && !device.contract.ok" class="banner" role="alert"><div><strong>{{ device.contract.reason }}</strong><p v-if="device.contract.action">{{ device.contract.action }}</p></div></div>
    <div v-if="device.connected && device.unprovisioned" class="banner warn" role="alert">
      <div><strong>Master has no radio key</strong><p>Set a radio key in Device Settings</p></div>
    </div>
    <div v-if="device.quarantine" class="banner warn" role="alert">
      <div><strong>{{ device.quarantine.count }} invalid event{{ device.quarantine.count === 1 ? "" : "s" }} quarantined</strong><p>{{ device.quarantine.reason }}</p></div>
    </div>
    <div v-if="device.verDrop" class="banner warn" role="alert"><div><strong>Sensor Radio Protocol Mismatch</strong><p>Use matching firmware on all boards</p></div></div>

    <QualityAlert />

    <main class="main-content">
      <router-view v-if="ready" v-slot="{ Component }">
        <keep-alive>
          <component :is="Component" />
        </keep-alive>
      </router-view>
      <div v-else class="empty-state">Loading…</div>
    </main>
  </div>
</template>

<style scoped>
.link-dot {
  margin-right: 0.25rem;
}
.banner {
  max-width: 1400px;
  margin: 1rem auto 0;
  padding: 0.6rem 1rem;
  border-radius: 8px;
  background: rgba(239, 68, 68, 0.12);
  border: 1px solid rgba(239, 68, 68, 0.4);
  color: var(--text-primary);
  font-size: 0.875rem;
  display: flex;
  align-items: center;
  gap: 0.75rem;
}
.banner > div, .banner > span { min-width: 0; overflow-wrap: anywhere; }
.banner p { margin: 0.25rem 0 0; }
.banner-close { flex-shrink: 0; }
.banner.warn {
  background: rgba(245, 158, 11, 0.12);
  border-color: rgba(245, 158, 11, 0.5);
}
.banner code {
  font-family: var(--font-mono);
}
.banner-close {
  margin-left: auto;
  border: none;
  background: transparent;
  color: inherit;
  font-size: 1.1rem;
  cursor: pointer;
}
@media (max-width: 1440px) {
  .banner {
    margin-left: 2rem;
    margin-right: 2rem;
  }
}
</style>
