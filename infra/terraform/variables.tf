variable "region" {
  type    = string
  default = "us-east-1"
}

variable "availability_zone" {
  description = "Empty = first available AZ in the region. c6in must be offered there."
  type        = string
  default     = ""
}

variable "instance_type" {
  description = <<-EOT
    Both nodes. c6in.2xlarge = 8 vCPU (4 cores x 2 threads), one socket, ENA up to
    50 Gbps. 4 physical cores leaves room for housekeeping + IRQs on core 0, the
    hot receive loop alone on another core with its hyperthread sibling idle.
    Single socket: there is no NUMA effect to measure, and the README does not
    claim one.
  EOT
  type        = string
  default     = "c6in.2xlarge"
}

variable "use_spot" {
  type    = bool
  default = true
}

variable "max_spot_price" {
  description = "USD/hour cap per instance. On-demand c6in.2xlarge is ~0.45."
  type        = string
  default     = "0.30"
}

variable "ttl_minutes" {
  description = "Each instance shuts itself down (and terminates) this long after boot."
  type        = number
  default     = 240
}

variable "allowed_ssh_cidr" {
  description = "Your public IP as a /32, e.g. \"203.0.113.7/32\"."
  type        = string
  validation {
    condition     = can(cidrhost(var.allowed_ssh_cidr, 0)) && var.allowed_ssh_cidr != "0.0.0.0/0"
    error_message = "Must be a CIDR, and not 0.0.0.0/0."
  }
}

variable "ssh_public_key_path" {
  type    = string
  default = "~/.ssh/id_ed25519.pub"
}

variable "repo_url" {
  type    = string
  default = "https://github.com/Other-Now/bypass-lab.git"
}

variable "repo_ref" {
  type    = string
  default = "main"
}

variable "hugepages_2m" {
  description = "2 MB huge pages reserved on each node (DPDK mempool + AF_XDP UMEM)."
  type        = number
  default     = 1024
}

variable "owner_tag" {
  type    = string
  default = "bypass-lab"
}
