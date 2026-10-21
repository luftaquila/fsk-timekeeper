<script setup>
import { onMounted, ref } from "vue";
import ThemeToggle from "./components/ThemeToggle.vue";
import NavTabs from "./components/NavTabs.vue";
import QualityAlert from "./components/QualityAlert.vue";
import MasterConnectionLight from "./components/MasterConnectionLight.vue";
import { useTimingStore } from "./stores/timing";
import { useDeviceStore } from "./stores/device";
import * as eventLog from "./lib/eventLog";
import * as ppsLog from "./lib/ppsLog";
import * as deviceLog from "./lib/deviceLog";

const timing = useTimingStore();
const device = useDeviceStore();
const ready = ref(false);

onMounted(async () => {
  const { durable, error } = await eventLog.init();
  device.durable = durable;
  device.durableError = error?.message || null;
  if (error) await deviceLog.addEntry({ kind: "console", code: "storage_failure", at: Date.now(), text: error.message || String(error) });
  device.requestPersistence();
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
          <a class="github-link" href="https://github.com/luftaquila/fsk-timekeeper" target="_blank" rel="noopener noreferrer" aria-label="GitHub repository (opens in a new tab)" title="GitHub">
            <svg viewBox="0 0 24 24" fill="currentColor" aria-hidden="true">
              <path d="M12 .5C5.37.5 0 5.87 0 12.5c0 5.3 3.438 9.8 8.205 11.385.6.113.82-.258.82-.577 0-.285-.01-1.04-.015-2.04-3.338.724-4.043-1.61-4.043-1.61-.546-1.387-1.333-1.756-1.333-1.756-1.09-.745.083-.729.083-.729 1.205.085 1.838 1.237 1.838 1.237 1.07 1.835 2.809 1.305 3.495.998.108-.776.418-1.305.762-1.605-2.665-.303-5.467-1.332-5.467-5.93 0-1.31.468-2.381 1.235-3.221-.123-.303-.535-1.524.117-3.176 0 0 1.008-.322 3.301 1.23a11.52 11.52 0 0 1 3.003-.404c1.02.005 2.047.138 3.003.404 2.291-1.552 3.297-1.23 3.297-1.23.654 1.652.242 2.873.12 3.176.77.84 1.233 1.911 1.233 3.221 0 4.61-2.807 5.624-5.479 5.921.43.372.823 1.102.823 2.222 0 1.606-.015 2.898-.015 3.293 0 .322.216.694.825.576C20.565 22.296 24 17.8 24 12.5 24 5.87 18.627.5 12 .5Z" />
            </svg>
          </a>
        </div>
      </div>
    </header>

    <div v-if="!device.durable" class="banner" role="alert">
      <div><strong>Browser storage unavailable</strong><p>Records will be lost on reload</p></div>
    </div>
    <div v-else-if="device.storageWarning" class="banner warn" role="alert">
      <div><strong>Browser storage is not protected from automatic cleanup</strong><p>Records may be lost when you close the browser. Always export your records before closing.</p></div>
    </div>
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
.github-link {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 40px;
  height: 40px;
  color: var(--text-secondary);
  background: var(--bg-primary);
  border: 1px solid var(--border-color);
  border-radius: 8px;
  transition: background-color 0.15s ease;
}
.github-link:hover { background: var(--bg-hover); }
.github-link svg { width: 20px; height: 20px; }
.banner {
  max-width: 1400px;
  margin: 1rem auto 0;
  padding: 0.6rem 1rem;
  border-radius: 8px;
  background: rgba(239, 68, 68, 0.12);
  border: 1px solid rgba(239, 68, 68, 0.4);
  color: var(--text-primary);
  font-size: 1rem;
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
