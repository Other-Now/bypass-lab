# Two EC2 instances in a cluster placement group, each with a second ENI that is
# the data plane. The primary ENI (management subnet) carries SSH and stays on
# the kernel driver; the data ENI (data subnet) is what the four receive paths
# fight over -- including being unbound from the kernel entirely for DPDK --
# so an experiment can never cut off its own SSH session.
#
#   make aws-up    -> terraform apply  (+ waits for cloud-init)
#   make aws-run   -> runs scripts/aws/run_aws.sh on the sender
#   make aws-fetch -> copies results back
#   make aws-down  -> terraform destroy
#
# Safety net: each instance schedules its own shutdown at boot (ttl_minutes)
# with shutdown behaviour = terminate, so a forgotten `make aws-down` costs at
# most ttl_minutes of two spot instances.

terraform {
  required_version = ">= 1.5"
  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.60"
    }
    tls = {
      source  = "hashicorp/tls"
      version = "~> 4.0"
    }
  }
}

provider "aws" {
  region = var.region
  default_tags {
    tags = {
      project = "bypass-lab"
      owner   = var.owner_tag
    }
  }
}

data "aws_availability_zones" "available" {
  state = "available"
}

locals {
  az = var.availability_zone != "" ? var.availability_zone : data.aws_availability_zones.available.names[0]
  nodes = {
    sender   = { mgmt_ip = "10.42.1.10", data_ip = "10.42.2.10", peer_ip = "10.42.2.20", peer_mgmt = "10.42.1.20" }
    receiver = { mgmt_ip = "10.42.1.20", data_ip = "10.42.2.20", peer_ip = "10.42.2.10", peer_mgmt = "10.42.1.10" }
  }
}

data "aws_ami" "ubuntu" {
  most_recent = true
  owners      = ["099720109477"] # Canonical
  filter {
    name   = "name"
    values = ["ubuntu/images/hvm-ssd-gp3/ubuntu-noble-24.04-amd64-server-*"]
  }
  filter {
    name   = "virtualization-type"
    values = ["hvm"]
  }
}

# --- network ---------------------------------------------------------------

resource "aws_vpc" "lab" {
  cidr_block           = "10.42.0.0/16"
  enable_dns_hostnames = true
  tags                 = { Name = "bypass-lab" }
}

resource "aws_internet_gateway" "lab" {
  vpc_id = aws_vpc.lab.id
}

resource "aws_subnet" "mgmt" {
  vpc_id                  = aws_vpc.lab.id
  cidr_block              = "10.42.1.0/24"
  availability_zone       = local.az
  map_public_ip_on_launch = true
  tags                    = { Name = "bypass-lab-mgmt" }
}

# Separate subnet so the kernel routes 10.42.2.0/24 out of the data ENI and
# nothing else, with no policy routing needed.
resource "aws_subnet" "data" {
  vpc_id            = aws_vpc.lab.id
  cidr_block        = "10.42.2.0/24"
  availability_zone = local.az
  tags              = { Name = "bypass-lab-data" }
}

resource "aws_route_table" "mgmt" {
  vpc_id = aws_vpc.lab.id
  route {
    cidr_block = "0.0.0.0/0"
    gateway_id = aws_internet_gateway.lab.id
  }
}

resource "aws_route_table_association" "mgmt" {
  subnet_id      = aws_subnet.mgmt.id
  route_table_id = aws_route_table.mgmt.id
}

resource "aws_security_group" "mgmt" {
  name   = "bypass-lab-mgmt"
  vpc_id = aws_vpc.lab.id
  ingress {
    description = "SSH from the operator"
    from_port   = 22
    to_port     = 22
    protocol    = "tcp"
    cidr_blocks = [var.allowed_ssh_cidr]
  }
  ingress {
    description = "everything between the two lab nodes"
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    self        = true
  }
  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }
}

resource "aws_security_group" "data" {
  name   = "bypass-lab-data"
  vpc_id = aws_vpc.lab.id
  ingress {
    from_port = 0
    to_port   = 0
    protocol  = "-1"
    self      = true
  }
  egress {
    from_port = 0
    to_port   = 0
    protocol  = "-1"
    self      = true
  }
}

resource "aws_placement_group" "lab" {
  name     = "bypass-lab"
  strategy = "cluster"
}

# --- keys ------------------------------------------------------------------

resource "aws_key_pair" "operator" {
  key_name   = "bypass-lab-operator"
  public_key = file(pathexpand(var.ssh_public_key_path))
}

# Sender -> receiver control channel (run_aws.sh starts bl_rx over SSH).
# Ephemeral: lives in this state file and dies with `terraform destroy`.
resource "tls_private_key" "peer" {
  algorithm = "ED25519"
}

# --- instances -------------------------------------------------------------

resource "aws_network_interface" "data" {
  for_each          = local.nodes
  subnet_id         = aws_subnet.data.id
  private_ips       = [each.value.data_ip]
  security_groups   = [aws_security_group.data.id]
  source_dest_check = true
  tags              = { Name = "bypass-lab-data-${each.key}" }
}

resource "aws_instance" "node" {
  for_each                             = local.nodes
  ami                                  = data.aws_ami.ubuntu.id
  instance_type                        = var.instance_type
  subnet_id                            = aws_subnet.mgmt.id
  private_ip                           = each.value.mgmt_ip
  vpc_security_group_ids               = [aws_security_group.mgmt.id]
  key_name                             = aws_key_pair.operator.key_name
  placement_group                      = aws_placement_group.lab.id
  instance_initiated_shutdown_behavior = "terminate"

  dynamic "instance_market_options" {
    for_each = var.use_spot ? [1] : []
    content {
      market_type = "spot"
      spot_options {
        max_price                      = var.max_spot_price
        spot_instance_type             = "one-time"
        instance_interruption_behavior = "terminate"
      }
    }
  }

  root_block_device {
    volume_size = 30
    volume_type = "gp3"
  }

  metadata_options {
    http_tokens = "required"
  }

  user_data = templatefile("${path.module}/bootstrap.sh.tftpl", {
    role         = each.key
    data_ip      = each.value.data_ip
    peer_data_ip = each.value.peer_ip
    peer_mgmt_ip = each.value.peer_mgmt
    data_mac     = aws_network_interface.data[each.key].mac_address
    peer_mac     = aws_network_interface.data[each.key == "sender" ? "receiver" : "sender"].mac_address
    peer_privkey = each.key == "sender" ? tls_private_key.peer.private_key_openssh : ""
    peer_pubkey  = tls_private_key.peer.public_key_openssh
    repo_url     = var.repo_url
    repo_ref     = var.repo_ref
    ttl_minutes  = var.ttl_minutes
    hugepages    = var.hugepages_2m
  })
  user_data_replace_on_change = true

  tags = { Name = "bypass-lab-${each.key}" }
}

resource "aws_network_interface_attachment" "data" {
  for_each             = local.nodes
  instance_id          = aws_instance.node[each.key].id
  network_interface_id = aws_network_interface.data[each.key].id
  device_index         = 1
}
