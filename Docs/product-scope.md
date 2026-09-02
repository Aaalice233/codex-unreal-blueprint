# Product scope

[简体中文](product-scope.zh-CN.md)

## Status

This document defines the intended v1.0.0 scope. It is not an availability statement. See the [release gate](v1-release-gate.md) for the evidence required before features can be described as released.

## v1.0.0 target

- Unreal Engine 4.27 on Win64.
- Standard Blueprint assets and Class Defaults.
- SCS components, inheritance overrides, attachment, transforms, reflected properties, and asset references.
- Variables and full `FEdGraphPinType` coverage.
- Event, Construction, Function, Macro, Delegate, Interface, and LevelScript graphs.
- Action Catalog-based nodes, pins, links, defaults, comments, reroutes, refresh, and deterministic layout.
- Functions, macros, custom events, dispatchers, interfaces, Function Libraries, and Macro Libraries.
- WidgetBlueprint WidgetTree, slots, properties, bindings, navigation, accessibility, and Widget Animation.
- AnimBlueprint AnimGraph, state machines, transitions, conduits, Pose Links, variables, and Event Graph.
- UserDefinedStruct and UserDefinedEnum.
- Create, duplicate, rename, move, delete, reparent, interface changes, Redirector repair, inspect, validate, apply, verify, and recover.

Every supported write operation must use the same Operation Registry and the complete preflight-to-recovery pipeline. Coverage without that pipeline does not satisfy v1.

## Explicit non-goals

The first release does not promise UE5, Material, Niagara, Sequencer, level Actor automation, MCP, a public TypeScript SDK, a standalone GUI, or an Unreal Dock panel. The Unreal integration is limited to a status icon and Tooltip; management belongs in Pi and the CLI.

Long-lived versioned plan files are not part of the product. Apply accepts an ordinary request for that job; durable state is limited to Job Journal and recovery/audit records.
