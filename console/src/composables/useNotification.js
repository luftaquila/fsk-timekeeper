import { Notyf } from "notyf";

const notyf = new Notyf({
  duration: 3500,
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

export function useNotification() {
  return notyf;
}
