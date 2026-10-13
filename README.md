- 100% C++, standalone, no dependencies
- persist previous session
- reschedule shutdown (overwritre on-going timer)
- Scroll to adjust
- estimated time to shutdown

<img width="320" height="221" alt="2026-10-14_03-46-21" src="https://github.com/user-attachments/assets/f40f315b-2204-4429-99fc-dee27a4d4796" />
All time fields accept large values such as `4000` seconds or minutes. The combined
delay is limited to Windows' supported maximum of 315,360,000 seconds (10 years).
