import { Notyf } from "notyf";
import { ref, watch, onScopeDispose } from "vue";

export const NOTIFICATION_DURATION = 3500;
export const notificationText = (text) => String(text ?? "").replace(/\.\s*$/, "");

const notyf = new Notyf({
  duration: NOTIFICATION_DURATION,
  position: { x: "right", y: "top" },
  ripple: false,
  dismissible: true,
  types: [
    {
      type: "warning",
      background: "#f59e0b",
      icon: false,
    },
  ],
});

const notifications = {
  success: (message) => notyf.success(notificationText(message)),
  error: (message) => notyf.error(notificationText(message)),
};

export function useNotification() { return notifications; }

export function useActionResult() {
  const result = ref(null);
  let timer;
  watch(result, (value) => {
    clearTimeout(timer);
    if (value != null) timer = setTimeout(() => { result.value = null; }, NOTIFICATION_DURATION);
  }, { flush: "sync" });
  onScopeDispose(() => clearTimeout(timer));
  return result;
}
