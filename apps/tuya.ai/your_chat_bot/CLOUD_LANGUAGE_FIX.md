# Cloud language and robot-action configuration

The firmware does not automatically upload `ROBOT_UART_AI_SYSTEM_PROMPT_EXTRA`. Apply the following rules to the
agent version that is actually bound to the product/device.

## Agent prompt

Append this text in **02 Prompt Development**:

```text
# Reply language (highest priority)
- Determine the reply language only from the user's latest ASR/request.
- Never select the reply language from older conversation turns, retrieved knowledge, long-term memory, examples,
  tool descriptions, tool arguments, or tool results.
- If the latest request is English, every user-facing sentence must be English, including acknowledgements before a
  tool call and completion messages after it.
- If the latest request is Chinese, reply in Chinese.
- Internal text such as "User Problem", "Relevant information", command codes, and tool results is context, not a
  user language preference.
- If the latest request is ambiguous but contains English words and no Chinese characters, reply in English.

# Robot action correctness
- Use self_robot_send_command for explicit robot body motions.
- spin, rotate, or turn around -> kvtL 360 (or kvtR 360).
- go/walk/move forward -> kwkF <steps>.
- Never use kwkF for a spin and never claim that forward walking completed a spin.
- Say an action is complete only after the tool reports that the robot completion token was received.
- Do not expose command codes or internal tool/context text in user-facing replies.
```

## Platform steps

1. Open **AI Agent > Agent Dev > My Agent**, then develop the agent bound to this PID.
2. In **01 Model Configuration**, set voice interaction/role language to English for an English-only product.
3. Temporarily disable long-term memory and conversation-history retrieval for diagnosis. Clear the device's chat
   context, conversation summaries, and long-term memories before retesting.
4. Add the prompt above in **02 Prompt Development**. Also remove older examples that say `向前走三步` or otherwise
   mix Chinese into English-only response examples.
5. Verify that `self_robot_send_command` is present in the tool list and publish a new agent version.
6. Confirm that the new version is bound to the same product/device being tested. A draft version does not affect the
   released device.

## Regression test

Run these in one wake session, then repeat after exiting and waking again:

1. `Please walk forward three steps.` Expected command: `kwkF 3`; reply: English.
2. `Please spin around.` Expected command: `kvtL 360` or `kvtR 360`; reply: English.
3. `You're not spinning. Can you try again?` Expected command: a turn command, never `kwkF`; reply: English.
4. Ask an ambiguous English follow-up such as `How about now?` after a tool call. All NLG chunks must remain English.
5. If a wrong-language response is returned, firmware should log
   `robot voice: suppress NLG/TTS whose language conflicts with latest ASR` and resume listening after the response
   stream ends.

