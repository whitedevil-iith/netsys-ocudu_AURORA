#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

import bson
import click
import pymongo
import random
import sys

from misc.db.python.Open5GS import Open5GS

# Open5GS unit 3 = Gbps. Session and UE AMBR both need this or SMF/AMF will cap at 1 Gbps.
_AMBR_10GBPS = {"uplink": {"value": 10, "unit": 3}, "downlink": {"value": 10, "unit": 3}}


def _slice_entry(slice_sst, normalized_sd, default_indicator, apn, qci, ip_alloc):
    """Build one subscribed slice; an empty normalized_sd omits the SD."""
    entry = {
        "sst": int(slice_sst),
        "default_indicator": default_indicator,
        "session": [
            {
                "name": apn,
                "type": 3,
                "pcc_rule": [],
                "ambr": _AMBR_10GBPS,
                "qos": {
                    "index": int(qci),
                    "arp": {"priority_level": 8, "pre_emption_capability": 1, "pre_emption_vulnerability": 1},
                },
                "ue": {"ipv4": ip_alloc},
            },
            {
                "name": "ims",
                "type": 3,
                "pcc_rule": [],
                "ambr": _AMBR_10GBPS,
                "qos": {
                    "index": 5,
                    "arp": {"priority_level": 8, "pre_emption_capability": 1, "pre_emption_vulnerability": 1},
                },
            },
        ],
    }
    if normalized_sd:
        entry["sd"] = normalized_sd
    return entry


def add_user(
    imsi,
    key="00112233445566778899aabbccddeeff",
    op=None,
    opc="63bfa50ee6523365ff14c1f45f88737d",
    amf="9001",
    apn="internet",
    qci="9",
    ip_alloc="",
    access_restriction_data="0",
    slice_sst="1",
    slice_sd="",
):
    """Add UE data to Open5GS mongodb.

    slice_sd accepts one hex SD or multiple SDs separated by "|". The first
    listed slice is the default. Explicit SD subscriptions are required for
    UEs that request an SD; without them IMSI 001010123456789 repeatedly
    received "DNN Not Supported OR Not Subscribed in the Slice".
    """

    if op is not None:
        opc = None

    slice_sds = [part.strip() for part in str(slice_sd).split("|") if part.strip()]
    if slice_sds:
        slice_data = [
            _slice_entry(slice_sst, sd.lower().replace("0x", ""), index == 0, apn, qci, ip_alloc)
            for index, sd in enumerate(slice_sds)
        ]
    else:
        slice_data = [_slice_entry(slice_sst, "", True, apn, qci, ip_alloc)]

    sub_data = {
        "imsi": imsi,
        "subscribed_rau_tau_timer": 12,
        "network_access_mode": 2,
        "subscriber_status": 0,
        "access_restriction_data": int(access_restriction_data),
        "slice": slice_data,
        "ambr": _AMBR_10GBPS,
        "security": {"k": key, "amf": amf, "op": op, "opc": opc},
        "schema_version": 1,
        "__v": 0,
    }

    return sub_data


def read_from_db(db_file):
    """Read UE data from a subscriber db csv-file."""
    subscriber_db = []
    try:
        db_file = open(db_file, "r")
    except FileNotFoundError as e:
        print(f"Error reading subscriber_db.csv: {e}")
        return None

    for line in db_file:
        if not line.strip() or line.lstrip().startswith("#"):
            pass
        else:
            fields = line.rstrip().split(",")
            if len(fields) < 8:
                print(f"Error reading subscriber_db.csv: expected at least 8 fields, got {len(fields)}")
                return None

            name, imsi, key, op_type, op_c, amf, qci, ip_alloc = fields[:8]
            apn = fields[8] if len(fields) >= 9 and fields[8] else "internet"
            access_restriction_data = fields[9] if len(fields) >= 10 and fields[9] else "0"
            slice_sst = fields[10] if len(fields) >= 11 and fields[10] else "1"
            slice_sd = fields[11] if len(fields) >= 12 and fields[11] else ""

            opc = op_c
            op = None
            if op_type == "op":
                op = op_c
                opc = None

            subscriber_db.append(
                {
                    "imsi": imsi,
                    "key": key,
                    "op": op,
                    "opc": opc,
                    "amf": amf,
                    "qci": qci,
                    "ip_alloc": ip_alloc.rstrip(),
                    "apn": apn,
                    "access_restriction_data": access_restriction_data,
                    "slice_sst": slice_sst,
                    "slice_sd": slice_sd,
                }
            )

    return subscriber_db


def read_from_string(sub_data):
    """
    Read UE data from subscriber data string.
    Example string: "001010123456780,00112233445566778899aabbccddeeff,opc,63bfa50ee6523365ff14c1f45f88737d,8000,9,10.45.1.2"
    """

    subscriber_db = []

    fields = sub_data.split(",")
    if len(fields) < 7:
        print(f"Error reading subscriber string: expected at least 7 fields, got {len(fields)}")
        return None

    imsi, key, op_type, op_c, amf, qci, ip_alloc = fields[:7]
    apn = fields[7] if len(fields) >= 8 and fields[7] else "internet"
    access_restriction_data = fields[8] if len(fields) >= 9 and fields[8] else "0"
    slice_sst = fields[9] if len(fields) >= 10 and fields[9] else "1"
    slice_sd = fields[10] if len(fields) >= 11 and fields[10] else ""

    opc = op_c
    op = None
    if op_type == "op":
        op = op_c
        opc = None

    subscriber_db.append(
        {
            "imsi": imsi,
            "key": key,
            "op": op,
            "opc": opc,
            "amf": amf,
            "qci": qci,
            "ip_alloc": ip_alloc.rstrip(),
            "apn": apn,
            "access_restriction_data": access_restriction_data,
            "slice_sst": slice_sst,
            "slice_sd": slice_sd,
        }
    )

    return subscriber_db


@click.command()
@click.option("--mongodb", default="127.0.0.1", help="IP address or hostname of the mongodb instance.")
@click.option("--mongodb_port", default=27017, help="Port to connect to the mongodb instance.")
@click.option(
    "--subscriber_data",
    default="001010123456780,00112233445566778899aabbccddeeff,opc,63bfa50ee6523365ff14c1f45f88737d,8000,9,10.45.1.2",
    help="Single subscriber data string or full path to subscriber data csv-file.",
)
def main(mongodb, mongodb_port, subscriber_data):

    open5gs_client = Open5GS(mongodb, mongodb_port)

    if subscriber_data.endswith(".csv"):
        print("Reading subscriber data from csv-file.")
        subscriber_db = read_from_db(subscriber_data)
    else:
        print("Reading subscriber data from cmd.")
        subscriber_db = read_from_string(subscriber_data)

    if not subscriber_db:
        return sys.exit(1)

    for ue in subscriber_db:
        try:
            sub_data = add_user(**ue)
            # Add Subscriber using dict of sub_data
            print(open5gs_client.AddSubscriber(sub_data))
        except pymongo.errors.DuplicateKeyError:
            print(f"UE (IMSI={ue['imsi']}) already exists, updating it.")
            sub_data = add_user(**ue)
            # Update Subscriber using dict of sub_data
            print(open5gs_client.UpdateSubscriber(ue["imsi"], sub_data))


if __name__ == "__main__":
    main()
