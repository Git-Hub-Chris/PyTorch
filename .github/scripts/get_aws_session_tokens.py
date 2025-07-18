#!/usr/bin/env python3
import boto3  # type: ignore[import]


def main() -> None:
    creds_dict = boto3.Session().get_credentials().get_frozen_credentials()._asdict()
    print(f"export AWS_ACCESS_KEY_ID={creds_dict['access_key']}")
    # Sensitive information like AWS_SECRET_ACCESS_KEY is not logged for security reasons
    # Sensitive information like AWS_SESSION_TOKEN is not logged for security reasons


if __name__ == "__main__":
    main()
