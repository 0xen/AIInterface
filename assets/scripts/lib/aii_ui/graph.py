"""`NodeGraph` -- boxes with pins, wired together, drawn with `aii.ui`'s node
editor ops (`begin_node_editor` .. `end_node_editor`).

This is a small model of a node graph (nodes, their input/output pins, links
between pins) plus a `draw(ui)` that renders it and folds the user's edits
-- drags, new links, deleted links, selection -- back into the model every
frame. It is the node-graph equivalent of `StatusPanel`: reach for this
first, and only drop to the raw `aii.ui` node calls (documented in
`AII-UI.md`) for something this does not cover.

The usual shape, inside a `Panel.run`'s `draw(ui)`:

    from aii_ui import NodeGraph

    g = NodeGraph()
    mic = g.add_node("Microphone", outputs=["audio"])
    rec = g.add_node("Recogniser", inputs=["audio"], outputs=["text"])
    g.add_link(g.pin(mic, "audio"), g.pin(rec, "audio"))
    g.layout_grid()

    def draw(ui):
        g.draw(ui)

A graph read back from a state file (`aii_ui.state.watch_json`) is a
`to_dict()`/`from_dict()` round trip -- that is the hand-off this module
exists for: the assistant writes `scripts\\state\\<key>.json` with a graph
in that shape, a panel's `draw` turns it into a `NodeGraph` with
`NodeGraph.from_dict()` and shows it.

Extend this file the way the rest of `aii_ui` is extended: a graph that
wants something this does not have (per-node context menus, saved layouts)
gets a new method here, not a second implementation inline in a panel.
"""


# Node, pin and link ids, for every NodeGraph in this app session.
#
# They come from one counter, kept on the `aii` module so it survives this
# module being reloaded. That makes them unique for the whole session, not
# just within one graph. The window reports each node's position under its
# id, and a window closed and reopened would otherwise be handed the old
# window's reports; a new graph that reused the same ids would then take
# every node as already placed and pile them all at the origin.
#
# They also stay below 2**24. The app carries a link's two pin ids as 32-bit
# floats (`ui.link` -> UiCommand::f), and a float holds a whole number exactly
# only up to 16,777,216. Past that, the pin ids are rounded, the link names
# pins that do not exist, and imnodes draws no line -- so ids are kept well
# under that limit rather than given a much larger range.
ID_LIMIT = 1 << 24


def _session_id():
    import aii
    n = getattr(aii, "_nodegraph_next_id", 1)
    if n >= ID_LIMIT:
        n = 1   # after 16.7 million ids: start again rather than lose links
    aii._nodegraph_next_id = n + 1
    return n


class NodeGraph:
    """A node graph: nodes with labelled input/output pins, links between
    pins, drawn and edited through `aii.ui`'s node editor calls.

    Ids (nodes, pins, links) all come from one internal counter, because
    imnodes requires every id in one editor -- node, attribute and link
    alike -- to be unique. Never hand your own ids to `add_node`/`add_link`;
    let the graph allocate them and look pins up with `pin()`.
    """

    def __init__(self, minimap=True, auto_space=True, spacing=28.0):
        # `auto_space`: once a node has been drawn and its real size is known,
        # any node overlapping an earlier one is pushed right or down (the
        # shorter move) until nothing overlaps, with `spacing` pixels between.
        # It runs once per node, on its first frames, and never again -- so a
        # user who drags two nodes together afterwards is left alone. Without
        # it, nodes with hint-heavy bodies laid out on a grid sized for bare
        # titles can end up sitting on top of each other.
        self.minimap = minimap
        self.auto_space = auto_space
        self.spacing = float(spacing)
        self._size = {}       # id -> (w, h) as last drawn
        self._settle = set()  # node ids still owed a spacing pass
        # None: ids come from the session counter above. A test may pin
        # them by setting an int here; it must stay below ID_LIMIT.
        self._next_id = None
        self._nodes = {}      # id -> node dict
        self._node_order = []
        self._links = {}      # id -> (start_attr, end_attr)
        self._pin_of = {}     # (node_id, "in"/"out", label) -> attr id
        # Node ids the app has confirmed it drew at their position. A position
        # is re-sent every frame until then (see draw()), because a recording
        # is not a render: a node inside a closed tab or collapsed header is
        # recorded but never drawn, and its SetNodePos never reaches imnodes.
        self._placed = set()
        self._force = {}      # id -> (x, y) forced move not yet confirmed drawn
        self._selected = []
        self.on_link = None       # (start, end) -> bool | None
        self.on_unlink = None     # (link_id) -> None
        self.on_select = None     # (node_ids) -> None

    # ---- building -----------------------------------------------------

    def _alloc(self):
        if self._next_id is None:
            return _session_id()
        i = self._next_id
        if i >= ID_LIMIT:
            raise ValueError("node graph id %d is not below %d: the app carries link ends as "
                             "32-bit floats, so the link would be lost" % (i, ID_LIMIT))
        self._next_id += 1
        return i

    @staticmethod
    def _split(spec):
        """A pin spec is a label, or (label, shape)."""
        if isinstance(spec, (tuple, list)):
            return spec[0], spec[1]
        return spec, 1  # PIN_CIRCLE_FILLED-ish default; ui.py supplies the real default

    def add_node(self, title, inputs=(), outputs=(), pos=None, color=None, body=None):
        """Add a node and return its id. `inputs`/`outputs` are pin labels
        (str) or (label, shape) pairs. `body(ui, node)` is called between the
        input and output pins for per-node widgets -- a slider, a checkbox --
        and may be None. `pos` is (x, y) in grid space, or None to leave the
        node wherever `layout_grid()` or the user puts it."""
        node_id = self._alloc()
        node = {
            "id": node_id,
            "title": title,
            "inputs": [],
            "outputs": [],
            "pos": tuple(pos) if pos else None,
            "color": tuple(color) if color else None,
            "size": None,
            "body": body,
        }
        for spec in inputs:
            label, shape = self._split(spec)
            attr_id = self._alloc()
            node["inputs"].append({"id": attr_id, "label": label, "shape": shape})
            self._pin_of[(node_id, "in", label)] = attr_id
        for spec in outputs:
            label, shape = self._split(spec)
            attr_id = self._alloc()
            node["outputs"].append({"id": attr_id, "label": label, "shape": shape})
            self._pin_of[(node_id, "out", label)] = attr_id
        self._nodes[node_id] = node
        self._node_order.append(node_id)
        self._settle.add(node_id)
        return node_id

    def remove_node(self, node_id):
        """Remove a node and every link touching one of its pins."""
        node = self._nodes.pop(node_id, None)
        if node is None:
            return
        self._node_order.remove(node_id)
        self._placed.discard(node_id)
        self._force.pop(node_id, None)
        attrs = {p["id"] for p in node["inputs"]} | {p["id"] for p in node["outputs"]}
        for key in [k for k in self._pin_of if k[0] == node_id]:
            del self._pin_of[key]
        for link_id, (s, e) in list(self._links.items()):
            if s in attrs or e in attrs:
                del self._links[link_id]

    def pin(self, node_id, label):
        """The attribute id for `node_id`'s pin named `label`, input or
        output, whichever exists. Raises KeyError if there is no such pin."""
        for side in ("in", "out"):
            attr_id = self._pin_of.get((node_id, side, label))
            if attr_id is not None:
                return attr_id
        raise KeyError("no pin '%s' on node %s" % (label, node_id))

    def add_link(self, start_attr, end_attr):
        """Add a link between two attribute ids and return its id. This is
        the model-side call: use it directly to build a graph's starting
        links, or let `draw()` call it for a link the user just made (unless
        `on_link` returns False)."""
        link_id = self._alloc()
        self._links[link_id] = (start_attr, end_attr)
        return link_id

    def remove_link(self, link_id):
        self._links.pop(link_id, None)

    def links(self):
        """[(id, start_attr, end_attr), ...]"""
        return [(lid, s, e) for lid, (s, e) in self._links.items()]

    def nodes(self):
        """The node dicts, in the order they were added."""
        return [self._nodes[i] for i in self._node_order]

    def selected_nodes(self):
        return list(self._selected)

    # ---- layout ---------------------------------------------------------

    def layout_grid(self, columns=3, dx=300, dy=180):
        """Give a grid position to every node that does not have one yet,
        so a graph built from `from_dict()` (or from `add_node()` calls with
        no `pos`) lands somewhere sensible instead of stacked at the origin."""
        col = row = 0
        for node_id in self._node_order:
            node = self._nodes[node_id]
            if node["pos"] is None:
                node["pos"] = (float(col * dx), float(row * dy))
                self._placed.discard(node_id)
                col += 1
                if col >= columns:
                    col = 0
                    row += 1

    # ---- drawing ----------------------------------------------------------

    def draw(self, ui):
        """Apply the previous frame's edits (new links, deleted links, node
        drags, selection), then record this frame's editor. Call this once
        per tick from a `Panel`'s `draw(ui)`."""
        self._apply_edits(ui)
        moves = self._space_out(ui) if self.auto_space else {}

        ui.begin_node_editor()

        for node_id in self._node_order:
            node = self._nodes[node_id]
            # Positions are sent every frame until the app reports the node
            # drawn there (the loop after end_node_editor). Marking them sent
            # on the first *recording* would lose them whenever that
            # recording is not drawn -- a panel can record a tab while it is
            # closed, so a freshly opened window would stack every node at
            # the origin and then save those zeros as the model's positions.
            # Re-sending is free: the app applies a non-forced SetNodePos
            # once per node id and ignores the rest.
            if node_id in moves:
                node["pos"] = moves[node_id]
                self._force[node_id] = moves[node_id]
            if node_id in self._force:
                x, y = self._force[node_id]
                ui.set_node_pos(node_id, x, y, force=True)
            elif node["pos"] is not None and node_id not in self._placed:
                ui.set_node_pos(node_id, node["pos"][0], node["pos"][1])

            pushed_color = node["color"] is not None
            if pushed_color:
                ui.push_node_color(ui.NODE_COL_TITLE_BAR, node["color"])

            ui.begin_node(node_id)

            ui.begin_node_title_bar()
            ui.text(node["title"])
            ui.end_node_title_bar()

            for attr in node["inputs"]:
                ui.begin_input_attribute(attr["id"], attr["shape"])
                ui.text(attr["label"])
                ui.end_input_attribute()

            if node["body"] is not None:
                node["body"](ui, node)

            for attr in node["outputs"]:
                ui.begin_output_attribute(attr["id"], attr["shape"])
                ui.text(attr["label"])
                ui.end_output_attribute()

            ui.end_node()

            if pushed_color:
                ui.pop_node_color()

        for link_id, (start, end) in self._links.items():
            ui.link(link_id, start, end)

        if self.minimap:
            ui.mini_map()

        ui.end_node_editor()

        # Grid-space positions as of this render, for next frame's model and
        # for to_dict() -- store them now while ui.node_pos still has them.
        # A node the app has not drawn yet has no reported position, and its
        # model position is kept; one still owed a position is not read back
        # until the report shows it landed, so an origin never overwrites it.
        for node_id in self._node_order:
            pos = ui.node_pos(node_id)
            if pos is not None:
                target = self._force.get(node_id)
                if target is not None:
                    if abs(pos[0] - target[0]) > 0.5 or abs(pos[1] - target[1]) > 0.5:
                        continue   # the forced move has not been drawn yet
                    del self._force[node_id]
                # Reported means drawn, and SetNodePos came before BeginNode
                # in that same frame, so this is where the node landed.
                self._placed.add(node_id)
                self._nodes[node_id]["pos"] = pos
            size = ui.node_size(node_id)
            if size is not None:
                self._nodes[node_id]["size"] = size

    def _space_out(self, ui):
        """The spacing pass. Returns {node_id: (x, y)} for nodes to move this
        frame, or {} when there is nothing to do yet or nothing left to do.
        Waits until every node still owed a pass has been drawn once (so its
        size is known); then pushes each overlapping later node right or
        down, whichever is the shorter move, and repeats until clean."""
        if not self._settle:
            return {}
        for node_id in self._settle:
            node = self._nodes.get(node_id)
            if node is None:
                continue
            if node["pos"] is None or node["size"] is None:
                return {}  # not drawn yet; try again next frame
        rects = {}
        for node_id in self._node_order:
            node = self._nodes[node_id]
            if node["pos"] is None or node["size"] is None:
                continue
            rects[node_id] = [node["pos"][0], node["pos"][1], node["size"][0], node["size"][1]]
        moved = set()
        gap = self.spacing
        order = [n for n in self._node_order if n in rects]
        for _ in range(64):
            clean = True
            for i, a in enumerate(order):
                ax, ay, aw, ah = rects[a]
                for b in order[i + 1:]:
                    if b not in self._settle and a not in self._settle:
                        continue
                    # Move the later one, unless only the earlier is unsettled.
                    mover = b if b in self._settle else a
                    other = a if mover == b else b
                    mx, my, mw, mh = rects[mover]
                    ox, oy, ow, oh = rects[other]
                    if mx >= ox + ow + gap or ox >= mx + mw + gap:
                        continue
                    if my >= oy + oh + gap or oy >= my + mh + gap:
                        continue
                    push_x = (ox + ow + gap) - mx
                    push_y = (oy + oh + gap) - my
                    if push_y <= push_x:
                        rects[mover][1] = my + push_y
                    else:
                        rects[mover][0] = mx + push_x
                    moved.add(mover)
                    clean = False
            if clean:
                break
        self._settle.clear()
        return {n: (rects[n][0], rects[n][1]) for n in moved}

    def _apply_edits(self, ui):
        for start, end in ui.links_created():
            add = True
            if self.on_link is not None:
                add = self.on_link(start, end) is not False
            if add:
                self.add_link(start, end)

        destroyed = ui.links_destroyed()
        for link_id in destroyed:
            self.remove_link(link_id)
            if self.on_unlink is not None:
                self.on_unlink(link_id)

        selected = ui.selected_nodes()
        if selected != self._selected:
            self._selected = list(selected)
            if self.on_select is not None:
                self.on_select(self._selected)

    # ---- serialisation --------------------------------------------------

    def to_dict(self):
        """The JSON-shaped dict `from_dict()` reads back: nodes (title,
        inputs, outputs, pos, color) and links (by pin label, not by the
        internal attribute ids, so it survives a graph being rebuilt with a
        fresh id counter)."""
        nodes = []
        id_index = {}
        for idx, node_id in enumerate(self._node_order):
            node = self._nodes[node_id]
            id_index[node_id] = idx
            nodes.append({
                "title": node["title"],
                "inputs": [p["label"] for p in node["inputs"]],
                "outputs": [p["label"] for p in node["outputs"]],
                "pos": list(node["pos"]) if node["pos"] is not None else None,
                "color": list(node["color"]) if node["color"] is not None else None,
            })

        attr_label = {}
        for node_id, node in self._nodes.items():
            for p in node["inputs"] + node["outputs"]:
                attr_label[p["id"]] = (id_index[node_id], p["label"])

        links = []
        for _lid, start, end in self.links():
            if start in attr_label and end in attr_label:
                links.append({"from": list(attr_label[start]), "to": list(attr_label[end])})

        return {"nodes": nodes, "links": links}

    @classmethod
    def from_dict(cls, d, minimap=True):
        """Build a `NodeGraph` from the shape `to_dict()` produces (or a
        hand-written state file in the same shape -- see `AII-UI.md`).
        Nodes with no `pos` are left unplaced; call `layout_grid()` after
        this if the caller has not given every node a position."""
        g = cls(minimap=minimap)
        node_ids = []
        for n in d.get("nodes", []):
            node_id = g.add_node(
                n.get("title", ""),
                inputs=n.get("inputs", []) or [],
                outputs=n.get("outputs", []) or [],
                pos=n.get("pos"),
                color=n.get("color"),
            )
            node_ids.append(node_id)
        for link in d.get("links", []):
            try:
                fi, flabel = link["from"]
                ti, tlabel = link["to"]
                start = g.pin(node_ids[fi], flabel)
                end = g.pin(node_ids[ti], tlabel)
            except (KeyError, IndexError, ValueError):
                continue
            g.add_link(start, end)
        return g
