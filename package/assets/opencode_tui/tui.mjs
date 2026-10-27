import { createEffect, createRoot } from "solid-js"
import { create_tui_integration } from "../logonomic_opencode_tui.mjs"

export default {
  id: "logonomic",
  async setup(context) {
    const integration = await create_tui_integration(context)
    if (!integration) return
    const stop_observing = createRoot((dispose) => {
      createEffect(() => integration.select())
      return dispose
    })
    return async () => {
      stop_observing()
      await integration.dispose()
    }
  },
}
