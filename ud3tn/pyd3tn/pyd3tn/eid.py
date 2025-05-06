# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
"""Helper functions for handling endpoint identifiers (EIDs)."""


def get_node_id(eid: str) -> str:
    """Return the node ID belonging to a given EID.

    Returns:
        For ipn EIDs, this returns "ipn:N.0" whereas "N" is the node number.
        For dtn EIDs, "dtn://node/" is returned, wheras "node" is the node
        name. On error, a ValueError is raised.
    """
    if eid[0:6] == "dtn://":
        split_eid = eid.split("/")
        if len(split_eid) > 3 and split_eid[3].startswith("~"):
            raise ValueError("Non-singleton EID - no unique node ID present.")
        if len(split_eid[2]) == 0:
            raise ValueError("No node identifier present in EID.")
        return "dtn://" + split_eid[2] + "/"
    elif eid[0:4] == "ipn:":
        split_eid = eid.split(".")
        if (len(split_eid) != 2 or not split_eid[0][4:].isdigit() or
                not split_eid[1].isdigit()):
            raise ValueError("Invalid ipn EID format.")
        return split_eid[0] + ".0"
    elif eid == "dtn:none":
        return eid
    else:
        raise ValueError("Cannot determine the node prefix for the given EID.")


def test_get_node_id():
    import pytest
    assert "dtn://ud3tn/" == get_node_id("dtn://ud3tn/a")
    assert "dtn://ud3tn/" == get_node_id("dtn://ud3tn/a/")
    assert "dtn://ud3tn/" == get_node_id("dtn://ud3tn/a/b")
    assert "dtn://ud3tn/" == get_node_id("dtn://ud3tn/")
    assert "dtn://ud3tn/" == get_node_id("dtn://ud3tn")
    with pytest.raises(ValueError):
        get_node_id("dtn://ud3tn/~a")
    with pytest.raises(ValueError):
        get_node_id("dtn:///")
    with pytest.raises(ValueError):
        get_node_id("dtn:///A")
    with pytest.raises(ValueError):
        get_node_id("dtn://")
    assert "dtn:none" == get_node_id("dtn:none")
    assert "ipn:1.0" == get_node_id("ipn:1.0")
    assert "ipn:1.0" == get_node_id("ipn:1.1")
    assert "ipn:1.0" == get_node_id("ipn:1.42424242")
    with pytest.raises(ValueError):
        get_node_id("ipn:1:33")
    with pytest.raises(ValueError):
        get_node_id("ipn:1.")
    with pytest.raises(ValueError):
        get_node_id("ipn:1")
    with pytest.raises(ValueError):
        get_node_id("invalid:scheme")
