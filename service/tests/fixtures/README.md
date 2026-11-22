# Fixtures

Synthetic Paperclip API responses in the shape the service reads. All ids,
names and titles are made up. Only the fields the service uses are kept.

| File | Endpoint |
|---|---|
| `runs_idle.json` | `GET /companies/{cid}/heartbeat-runs?agentId={id}&limit=10&summary=true` (a list) |
| `run_detail_done.json` | `GET /heartbeat-runs/{runId}` for a finished run |
| `issues.json` | `GET /companies/{cid}/issues?assigneeAgentId={id}&status=...` (a list) |
| `interactions_resolved.json` | `GET /issues/{iid}/interactions` (a list) |
| `approvals_decided.json` | `GET /companies/{cid}/approvals?status=approved` (a list) |

Field notes:
- **Run:** `status` (`running`, `queued`, `succeeded`, `failed`, `cancelled`),
  `startedAt`/`finishedAt` ISO-8601, `issueId`, `logBytes`.
- **Run detail** adds `currentToolName`, `currentStatusMessage` (Paperclip's
  own progress text; its default is "Receiving agent output"),
  `lastAssistantSnippet` (the agent's last words) and `lastEventAt` (touched on
  any output). They're null once the run finishes.
- **Log:** `GET /heartbeat-runs/{id}/log?offset=&limitBytes=` returns
  `{content, nextOffset}`. `content` is NDJSON of `{ts, stream, chunk}`, and a
  chunk can split a stream-json line.
- **Interaction:** `kind`, `status` (e.g. `pending`, `answered`, `expired`,
  `cancelled`), `resolvedAt`, `dismissedAt`.
- **Approval:** no issue link; tie it to the agent by `requestedByAgentId`.
- **Comment:** `createdByRunId` ties a comment to the run that posted it.
- `GET /agents/{id}/runtime-state` may be forbidden for an agent key, so `next` can be null.
