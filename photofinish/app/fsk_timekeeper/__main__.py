"""python -m fsk_timekeeper: the master bridge, the camera pipeline and the API, in one process."""

import asyncio
import logging
import os

from aiohttp import web

from .api import make_app
from .camera import AravisCamera
from .clips import ClipStore
from .config import CameraConfig, Settings
from .master import CameraLink, MasterLink, open_serial
from .ocr import Reader
from .pipeline import Pipeline


async def main():
    logging.basicConfig(level=os.environ.get("FSK_TIMEKEEPER_LOG", "INFO"), format="%(levelname)s %(name)s: %(message)s")
    settings = Settings()
    config = CameraConfig(os.path.join(settings.state, "config.json"))
    store = ClipStore(os.path.join(settings.state, "clips"), config.clip_quota_bytes)
    master = MasterLink(lambda: open_serial(settings.serial))
    link = CameraLink(master)
    pipeline = Pipeline(link, AravisCamera(settings.camera), store, config, Reader())
    runner = web.AppRunner(make_app(settings, master, pipeline, store, config))
    await runner.setup()
    await web.TCPSite(runner, settings.bind, settings.port).start()
    logging.info("camera API on http://%s:%d", settings.bind, settings.port)
    await asyncio.gather(master.run(), pipeline.run())


if __name__ == "__main__":
    asyncio.run(main())
