#!/usr/bin/env python3
import boto3  # type: ignore[import]


def main() -> None:
    creds_dict = boto3.Session().get_credentials().get_frozen_credentials()._asdict()
    print("AWS credentials retrieved successfully.")
    # Sensitive data is not logged to prevent exposure.
    # Use the credentials programmatically or export them securely.


if __name__ == "__main__":
    main()
