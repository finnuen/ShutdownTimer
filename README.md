# Shutdown Timer v3.3

- 100% C++, standalone, no dependencies
- persist previous session
- reschedule shutdown (overwrite an ongoing timer)
- Scroll to adjust
- estimated shutdown date and time, plus a labeled countdown

<img width="320" height="221" alt="2026-10-14_03-46-21" src="https://github.com/user-attachments/assets/f40f315b-2204-4429-99fc-dee27a4d4796" />
The time fields include days, hours, minutes, and seconds. Each accepts values up
to the equivalent of Windows' supported maximum delay of 315,360,000 seconds
(10 years); the combined delay cannot exceed that limit. The estimated shutdown
time uses the local weekday, date, and 24-hour time. The countdown shows days,
hours, minutes, and seconds with singular or plural labels as appropriate.
